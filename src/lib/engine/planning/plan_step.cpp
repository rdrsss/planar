/// @file plan_step.cpp
/// @brief Implementation of `planar.engine.planning.plan_step`.

module planar.engine.planning.plan_step;

import std;
import planar.db;
import planar.json_text;
import planar.policy;

namespace planar::engine::planning {

namespace {

using planar::json_text::json_string;
namespace audit = planar::policy::audit;

/// @brief Wrap an audit write in this module's error type.
/// @param conn An open database connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, plan_step_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(plan_step_error::audit_write_failed);
  }
  return {};
}

// SQLite extended result code this module distinguishes. Mirrored rather
// than pulling in <sqlite3.h> — same posture as plan.cpp, which never
// touches the raw C API either.
constexpr int k_sqlite_constraint_unique = 2067; // SQLITE_CONSTRAINT_UNIQUE

/// @brief Whether a failure was the `unique (plan_id, ordinal)` constraint.
/// @param err The driver error.
/// @return `true` on `SQLITE_CONSTRAINT_UNIQUE`.
auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

constexpr std::string_view k_select_columns =
    "select id, plan_id, ordinal, body, status, task_id, created_at, updated_at from plan_steps";

/// @brief Materialize one row from a stepped statement positioned on the
/// `k_select_columns` projection.
/// @param stmt The statement, positioned on a row.
/// @return The step, or `query_failed` when `status` holds a value outside
/// the four the CHECK constraint admits.
auto read_row(db::statement& stmt) -> std::expected<plan_step, plan_step_error> {
  const auto status = step_status_from_text(stmt.column_text(4));
  if (!status) {
    return std::unexpected(plan_step_error::query_failed);
  }
  return plan_step{
      .id      = stmt.column_int64(0),
      .plan_id = stmt.column_int64(1),
      .ordinal = stmt.column_int64(2),
      .body    = stmt.column_text(3),
      .status  = *status,
      // `is_null` FIRST: `column_int64` on a SQL NULL yields 0, which is a
      // legitimate id-shaped value. Reading it unconditionally would make
      // "unlinked" indistinguishable from "linked to task 0" in every
      // renderer and every row assertion downstream.
      .task_id    = stmt.is_null(5) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(5)},
      .created_at = stmt.column_text(6),
      .updated_at = stmt.column_text(7),
  };
}

/// @brief Whether a row with `id` exists in `table`.
/// @param conn An open database connection.
/// @param table The table name. A module-internal literal, never operator input.
/// @param id The row id to probe.
/// @return Whether the row exists, or `query_failed`.
auto row_exists(db::connection& conn, std::string_view table, std::int64_t id) -> std::expected<bool, plan_step_error> {
  auto stmt = conn.prepare(std::format("select 1 from {} where id = ?", table));
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(plan_step_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

/// @brief The shared body of `mark_step_done` and `skip_step`.
///
/// Reads the current row, applies the transition matrix, writes, then
/// audits — in that order, so a REFUSED transition leaves no `audit_log`
/// row. That ordering is the oracle's and is asserted directly.
/// @param conn An open database connection.
/// @param id The step's row id.
/// @param target The status to move to.
/// @return The updated row, or `not_found` / `invalid_transition`.
auto transition(db::connection& conn, std::int64_t id, step_status target) -> std::expected<plan_step, plan_step_error> {
  auto current = show_step(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (step_status_is_terminal(current->status)) {
    return std::unexpected(plan_step_error::invalid_transition);
  }
  // `skip` narrows further than `done`: only `pending` may be skipped, so
  // an `in_progress` step refuses here while the same step accepts `done`.
  if (target == step_status::skipped && current->status != step_status::pending) {
    return std::unexpected(plan_step_error::invalid_transition);
  }

  auto stmt = conn.prepare("update plan_steps set status = ?, "
                           "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, step_status_to_text(target)); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto stepped = stmt->step(); !stepped) {
    return std::unexpected(plan_step_error::query_failed);
  }

  // ORACLE: `status_change|plan_step|1|step 1: pending → done` — the arrow
  // is U+2192, not `->`. Captured from `audit_log.summary` after a real
  // `plan step done` against the reference binary.
  if (auto a = record_audit(
          conn,
          audit::record_args{
              .verb    = audit::verb::status_change,
              .entity  = {.kind = "plan_step", .id = id},
              .summary = std::format("step {}: {} → {}", id, step_status_to_text(current->status), step_status_to_text(target)),
          });
      !a) {
    return std::unexpected(a.error());
  }
  return show_step(conn, id);
}

} // namespace

auto step_status_from_text(std::string_view s) -> std::optional<step_status> {
  if (s == "pending") {
    return step_status::pending;
  }
  if (s == "in-progress") {
    return step_status::in_progress;
  }
  if (s == "done") {
    return step_status::done;
  }
  if (s == "skipped") {
    return step_status::skipped;
  }
  return std::nullopt;
}

auto step_status_to_text(step_status s) -> std::string_view {
  switch (s) {
  case step_status::pending:
    return "pending";
  case step_status::in_progress:
    return "in-progress";
  case step_status::done:
    return "done";
  case step_status::skipped:
    return "skipped";
  }
  return "pending";
}

auto step_status_is_terminal(step_status s) -> bool {
  return s == step_status::done || s == step_status::skipped;
}

auto add_step(db::connection& conn, const plan_step_add_args& args) -> std::expected<plan_step, plan_step_error> {
  auto exists = row_exists(conn, "plans", args.plan_id);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return std::unexpected(plan_step_error::not_found);
  }

  std::int64_t ordinal = 0;
  if (args.ordinal.has_value()) {
    ordinal = *args.ordinal;
  } else {
    auto stmt = conn.prepare("select coalesce(max(ordinal), 0) + 1 from plan_steps where plan_id = ?");
    if (!stmt) {
      return std::unexpected(plan_step_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, args.plan_id); !b) {
      return std::unexpected(plan_step_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(plan_step_error::query_failed);
    }
    // An aggregate always returns exactly one row, so `done` here would
    // mean the statement did not run at all — refuse rather than fall
    // through to ordinal 0, which the schema would then accept.
    if (*stepped != db::step_result::row) {
      return std::unexpected(plan_step_error::query_failed);
    }
    ordinal = stmt->column_int64(0);
  }

  auto stmt = conn.prepare("insert into plan_steps (plan_id, ordinal, body, status) "
                           "values (?, ?, ?, 'pending') returning id");
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, args.plan_id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, ordinal); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, args.body); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    if (is_unique_violation(stepped.error())) {
      return std::unexpected(plan_step_error::ordinal_conflict);
    }
    return std::unexpected(plan_step_error::query_failed);
  }
  const auto id = stmt->column_int64(0);

  // ORACLE: `create|plan_step|1|add step 1 to plan 1` — the ORDINAL, not
  // the step id, is the first number.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "plan_step", .id = id},
                                                     .summary = std::format("add step {} to plan {}", ordinal, args.plan_id)});
      !a) {
    return std::unexpected(a.error());
  }
  return show_step(conn, id);
}

auto show_step(db::connection& conn, std::int64_t id) -> std::expected<plan_step, plan_step_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(plan_step_error::not_found);
  }
  return read_row(*stmt);
}

auto list_steps(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<plan_step>, plan_step_error> {
  auto stmt = conn.prepare(std::format("{} where plan_id = ? order by ordinal", k_select_columns));
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, plan_id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  std::vector<plan_step> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(plan_step_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  return out;
}

auto mark_step_done(db::connection& conn, std::int64_t id) -> std::expected<plan_step, plan_step_error> {
  return transition(conn, id, step_status::done);
}

auto skip_step(db::connection& conn, std::int64_t id) -> std::expected<plan_step, plan_step_error> {
  return transition(conn, id, step_status::skipped);
}

auto link_step_task(db::connection& conn, std::int64_t step_id, std::int64_t task_id)
    -> std::expected<plan_step, plan_step_error> {
  auto current = show_step(conn, step_id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto task_there = row_exists(conn, "tasks", task_id);
  if (!task_there) {
    return std::unexpected(task_there.error());
  }
  if (!*task_there) {
    return std::unexpected(plan_step_error::not_found);
  }

  auto stmt = conn.prepare("update plan_steps set task_id = ?, "
                           "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, step_id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto stepped = stmt->step(); !stepped) {
    return std::unexpected(plan_step_error::query_failed);
  }

  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::link,
                                                     .entity  = {.kind = "plan_step", .id = step_id},
                                                     .summary = std::format("link step {} to task {}", step_id, task_id)});
      !a) {
    return std::unexpected(a.error());
  }
  return show_step(conn, step_id);
}

auto unlink_step_task(db::connection& conn, std::int64_t step_id) -> std::expected<plan_step, plan_step_error> {
  auto current = show_step(conn, step_id);
  if (!current) {
    return std::unexpected(current.error());
  }

  auto stmt = conn.prepare("update plan_steps set task_id = null, "
                           "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, step_id); !b) {
    return std::unexpected(plan_step_error::query_failed);
  }
  if (auto stepped = stmt->step(); !stepped) {
    return std::unexpected(plan_step_error::query_failed);
  }

  // The oracle writes summary NULL here — genuinely absent, not empty.
  if (auto a = record_audit(conn,
                            audit::record_args{
                                .verb    = audit::verb::unlink,
                                .entity  = {.kind = "plan_step", .id = step_id},
                                .summary = std::nullopt,
                            });
      !a) {
    return std::unexpected(a.error());
  }
  return show_step(conn, step_id);
}

auto render_step_text(const plan_step& s) -> std::string {
  std::string out;
  out += std::format("id:       {}\n", s.id);
  out += std::format("plan_id:  {}\n", s.plan_id);
  out += std::format("ordinal:  {}\n", s.ordinal);
  out += std::format("status:   {}\n", step_status_to_text(s.status));
  if (s.task_id.has_value()) {
    out += std::format("task_id:  {}\n", *s.task_id);
  }
  out += std::format("body:     {}\n", s.body);
  out += std::format("created:  {}\n", s.created_at);
  out += std::format("updated:  {}\n", s.updated_at);
  return out;
}

auto render_step_json(const plan_step& s, bool with_ok) -> std::string {
  std::string out = "{";
  if (with_ok) {
    out += R"("ok":true,)";
  }
  out += std::format(R"("id":{},"plan_id":{},"ordinal":{},"body":{},"status":"{}","task_id":{},)"
                     R"("created_at":{},"updated_at":{}}})",
                     s.id, s.plan_id, s.ordinal, json_string(s.body), step_status_to_text(s.status),
                     s.task_id.has_value() ? std::format("{}", *s.task_id) : std::string{"null"}, json_string(s.created_at),
                     json_string(s.updated_at));
  return out;
}

auto render_step_list_text(std::span<const plan_step> steps, std::int64_t plan_id) -> std::string {
  if (steps.empty()) {
    return std::format("no steps for plan {}\n", plan_id);
  }
  std::string out = std::format("{:<5}  {:<10}  {:<10}  {}\n", "ord", "id", "status", "body");
  for (const auto& s : steps) {
    // The `+` is NOT a typo and NOT a C++ embellishment — it reproduces the
    // oracle byte-for-byte. `plan/step/list.zig` formats these two columns
    // with `{d:<5}` / `{d:<10}`, and under the Zig version this tree pins
    // that spec emits a FORCED SIGN as well as the left alignment, so the
    // reference binary prints `+1` where every sibling list verb prints
    // `1`. Captured with `od -c` (176 bytes for the three-step fixture):
    //
    //   b'ord    id          status      body'
    //   b'+1     +1          done        first step'
    //   b'+5     +3          pending     explicit five  [task:1]'
    //
    // `plan list` uses `{d:>5}` and shows NO sign, which is what rules out
    // "all integers render this way" and localises it to `<`. This is a
    // rendering defect in the reference, reported as a divergence in this
    // cycle's coder report; it is REPRODUCED rather than quietly corrected
    // because D2 governs and because a silent one-verb deviation is
    // exactly what the differential lane exists to catch. Fixing it is an
    // operator decision, not a porter's.
    out += std::format("{:<+5}  {:<+10}  {:<10}  {}", s.ordinal, s.id, step_status_to_text(s.status), s.body);
    if (s.task_id.has_value()) {
      out += std::format("  [task:{}]", *s.task_id);
    }
    out += '\n';
  }
  return out;
}

auto render_step_list_json(std::span<const plan_step> steps) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_step_json(steps[i], false);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
