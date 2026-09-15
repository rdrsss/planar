/// @file task.cpp
/// @brief Implementation of `planar.engine.planning.task` (see task.cppm).

module;

module planar.engine.planning.task;

import std;
import planar.db;
import planar.json_text;
import planar.log;
import planar.scope_ref;
import planar.policy;
import planar.engine.planning.plan;
import planar.engine.planning.transitions;

namespace planar::engine::planning {

using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface.
///
/// ROW ORDER IS OBSERVABLE. Every caller below records the TASK's own row
/// BEFORE calling `recompute_plan`, because the oracle's `audit_log` shows
/// the task's `status_change` immediately followed by the plan's
/// `recompute plan ...` row, in that order (captured from `task block 1
/// --on 2`). Recording after the recompute would invert them, and nothing
/// rendered would change.
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, task_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(task_error::audit_write_failed);
  }
  return {};
}

/// @brief Emit the oracle's INNER statement-failure diagnostic and map the
/// failure to `query_failed` (task 6202).
///
/// The oracle wraps each write in `d.execParams(...) catch |e| { ...
/// std.log.err("<op> exec failed: {s}", .{@errorName(e)}); return
/// Error.QueryFailed; }`, so a failed write puts TWO lines on stderr — the
/// engine's, naming the operation and the underlying failure, then the
/// handler's `error: <verb>: QueryFailed`. Reproduced for `task add --plan
/// <dangling>`, whose foreign key fires at step time:
///
///     error: task.create exec failed: StepFailed
///     error: task add: QueryFailed
///
/// The `zig_error_name` argument is `@errorName(e)` from
/// `zig/src/db/sqlite.zig`'s `execParams`, which distinguishes exactly
/// three arms: `PrepareFailed` (:234), `BindFailed` (:247) and
/// `StepFailed` (:252). This tree splits prepare/bind/step into separate
/// `std::expected` arms already, so each one passes its own name rather
/// than deriving one from a collapsed error value.
///
/// THIS IS ONE SITE OF ~28. The same `<op> exec failed` seam appears
/// throughout the oracle's engine (`grep -rn "exec failed" zig/src/`:
/// plan.create, question.create, decision.create, scenario.create,
/// artifact.create, annotation.*, entitylink.*, project.*, association.*,
/// runs.* and more). Only `task.create` is wired here because only its
/// path is reachable from the state differential's sequence and pinned by
/// task 6202; the rest are latent divergences that fire the moment a write
/// on those paths fails. See this cycle's report for the follow-up.
/// @param op The oracle's operation name, e.g. `task.create`.
/// @param zig_error_name The `@errorName` the oracle would have formatted.
/// @return Always `task_error::query_failed`.
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> task_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return task_error::query_failed;
}

constexpr int k_sqlite_constraint_unique = 2067; // SQLITE_CONSTRAINT_UNIQUE

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

/// @brief Whether every character of `s` is an ASCII decimal digit.
///
/// Ports zig's `allDigits`. `std::isdigit` is deliberately not used: it is
/// locale-sensitive and UB on a negative `char`, and the oracle's
/// `std.ascii.isDigit` is neither.
/// @param s The span to test.
/// @return `true` when `s` is all digits (vacuously true when empty, as in
/// the oracle — every caller passes a fixed-width slice).
auto all_digits(std::string_view s) -> bool {
  return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

/// @brief Days in `month` of `year`, proleptic Gregorian. Ports zig's
/// `daysInMonth`.
/// @param year The year.
/// @param month The 1-based month.
/// @return The day count, or 0 for a month outside 1..12.
auto days_in_month(std::uint32_t year, std::uint32_t month) -> std::uint32_t {
  switch (month) {
  case 1:
  case 3:
  case 5:
  case 7:
  case 8:
  case 10:
  case 12:
    return 31;
  case 4:
  case 6:
  case 9:
  case 11:
    return 30;
  case 2:
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
  default:
    return 0;
  }
}

/// @brief Whether `s` is exactly a `YYYY-MM-DD` calendar date. Ports zig's
/// `isDateOnly`, including the real calendar check — `2026-02-30` is
/// rejected, not merely range-checked.
/// @param s The candidate.
/// @return `true` when valid.
auto is_date_only(std::string_view s) -> bool {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  if (!all_digits(s.substr(0, 4)) || !all_digits(s.substr(5, 2)) || !all_digits(s.substr(8, 2))) {
    return false;
  }
  auto const to_u32 = [](std::string_view d) {
    std::uint32_t v = 0;
    for (char c : d) {
      v = (v * 10) + static_cast<std::uint32_t>(c - '0');
    }
    return v;
  };
  auto const year  = to_u32(s.substr(0, 4));
  auto const month = to_u32(s.substr(5, 2));
  auto const day   = to_u32(s.substr(8, 2));
  return month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
}

/// @brief Whether `s` is a basic RFC3339 timestamp. Ports zig's
/// `isRfc3339Like` arm for arm: `YYYY-MM-DDTHH:MM:SS`, an OPTIONAL
/// fractional part of one or more digits, then either a literal `Z` that
/// must END the string or a `±HH:MM` offset that must be the final six
/// characters. A trailing space, a lone `+`, or `Z` followed by anything
/// all fail.
/// @param s The candidate.
/// @return `true` when valid.
auto is_rfc3339_like(std::string_view s) -> bool {
  if (s.size() < 20 || !is_date_only(s.substr(0, 10)) || s[10] != 'T') {
    return false;
  }
  if (!all_digits(s.substr(11, 2)) || s[13] != ':' || !all_digits(s.substr(14, 2)) || s[16] != ':') {
    return false;
  }
  if (!all_digits(s.substr(17, 2))) {
    return false;
  }
  auto const two = [&](std::size_t at) { return static_cast<std::uint32_t>((s[at] - '0') * 10 + (s[at + 1] - '0')); };
  if (two(11) > 23 || two(14) > 59 || two(17) > 59) {
    return false;
  }

  std::size_t idx = 19;
  if (idx < s.size() && s[idx] == '.') {
    idx += 1;
    auto const start = idx;
    while (idx < s.size() && s[idx] >= '0' && s[idx] <= '9') {
      idx += 1;
    }
    if (idx == start) {
      return false;
    }
  }
  if (idx >= s.size()) {
    return false;
  }
  if (s[idx] == 'Z') {
    return idx + 1 == s.size();
  }
  if (s[idx] != '+' && s[idx] != '-') {
    return false;
  }
  if (idx + 6 != s.size()) {
    return false;
  }
  if (!all_digits(s.substr(idx + 1, 2)) || s[idx + 3] != ':' || !all_digits(s.substr(idx + 4, 2))) {
    return false;
  }
  return two(idx + 1) <= 23 && two(idx + 4) <= 59;
}

/// @brief Whether `s` is an acceptable `due_at`. Ports zig's `parseDueAt`,
/// which accepts the EMPTY string unconditionally — an explicit `--due ""`
/// stores `''`, which is distinct from the NULL an absent `--due` stores.
/// @param s The candidate.
/// @return `true` when valid.
auto due_at_is_valid(std::string_view s) -> bool {
  return s.empty() || is_date_only(s) || is_rfc3339_like(s);
}

auto scope_kind_to_text(task_scope_kind k) -> std::string_view {
  switch (k) {
  case task_scope_kind::global:
    return "global";
  case task_scope_kind::association:
    return "association";
  case task_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` onto this module's
/// own `task_scope_kind`. A 1:1 mapping kept explicit so this module's
/// public API is unaffected by the extraction (plan 996 task 6089, D19).
auto to_task_kind(scope_ref::scope_kind k) -> task_scope_kind {
  switch (k) {
  case scope_ref::scope_kind::global:
    return task_scope_kind::global;
  case scope_ref::scope_kind::association:
    return task_scope_kind::association;
  case scope_ref::scope_kind::repo:
    return task_scope_kind::repo;
  }
  return task_scope_kind::global; // unreachable
}

auto to_task_error(scope_ref::error e) -> task_error {
  switch (e) {
  case scope_ref::error::query_failed:
    return task_error::query_failed;
  case scope_ref::error::slug_not_found:
    return task_error::slug_not_found;
  }
  return task_error::query_failed; // unreachable
}

/// @brief Resolve `scope` (when present) to a `(kind,id)` pair, else
/// global. Delegates to `planar.scope_ref::resolve` (plan 996 task 6089,
/// D19) for the `"global"` / `"repo:<slug>"` / `"assoc:<slug>"` /
/// bare-association grammar and its DB lookup — that algorithm now lives
/// in exactly one place, shared with `engine_identity`'s `resolve_slug`
/// and this bucket's own `plan.cpp`. Only the "no scope at all -> global"
/// fold stays local to this module (same as plan.cpp's own copy).
auto resolve_scope_or_global(db::connection& conn, const std::optional<std::string>& scope)
    -> std::expected<std::pair<task_scope_kind, std::optional<std::int64_t>>, task_error> {
  if (!scope.has_value()) {
    return std::make_pair(task_scope_kind::global, std::optional<std::int64_t>{});
  }
  auto resolved = scope_ref::resolve(conn, *scope);
  if (!resolved) {
    return std::unexpected(to_task_error(resolved.error()));
  }
  return std::make_pair(to_task_kind(resolved->kind), resolved->id);
}

auto map_plan_error(plan_error e) -> task_error {
  switch (e) {
  case plan_error::not_found:
    return task_error::not_found;
  case plan_error::slug_conflict:
    return task_error::slug_conflict;
  case plan_error::slug_not_found:
    return task_error::slug_not_found;
  case plan_error::invalid_parent_cycle:
  case plan_error::illegal_transition:
  case plan_error::unknown_status:
  case plan_error::query_failed:
    return task_error::query_failed;
  // A plan-side `audit_log` write failure reached through `recompute_plan`
  // stays an audit failure on the task side rather than collapsing into
  // the generic query bucket -- both spell `WriteFailed` to the operator.
  case plan_error::audit_write_failed:
    return task_error::audit_write_failed;
  }
  return task_error::query_failed;
}

constexpr std::string_view k_select_columns =
    "select id, scope_kind, scope_id, plan_id, parent_task_id, title, body, slug, status, priority, "
    "next_action, due_at, created_at, updated_at from tasks";

auto read_row(db::statement& stmt) -> std::expected<task, task_error> {
  const auto      scope_kind_text = stmt.column_text(1);
  task_scope_kind sk;
  if (scope_kind_text == "global") {
    sk = task_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = task_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = task_scope_kind::repo;
  } else {
    return std::unexpected(task_error::query_failed);
  }

  const auto status_text = stmt.column_text(8);
  const auto status      = task_status_from_text(status_text);
  if (!status.has_value()) {
    return std::unexpected(task_error::query_failed);
  }

  return task{
      .id             = stmt.column_int64(0),
      .scope_kind     = sk,
      .scope_id       = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .plan_id        = stmt.is_null(3) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(3)},
      .parent_task_id = stmt.is_null(4) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(4)},
      .title          = stmt.column_text(5),
      .body           = stmt.is_null(6) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(6)},
      .slug           = stmt.is_null(7) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(7)},
      .status         = *status,
      .priority       = stmt.column_int64(9),
      .next_action    = stmt.is_null(10) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(10)},
      .due_at         = stmt.is_null(11) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(11)},
      .created_at     = stmt.column_text(12),
      .updated_at     = stmt.column_text(13),
  };
}

/// @brief Insert a `task_reopens` row. Only called when the from-status
/// is terminal (done/cancelled) and the to-status is an open status
/// (todo/doing/blocked) — mirrors zig's guard at both call sites.
auto record_reopen(db::connection& conn, std::int64_t task_id, task_status from, task_status to, std::string_view source,
                   std::optional<std::string_view> reason) -> std::expected<void, task_error> {
  auto stmt = conn.prepare("insert into task_reopens (task_id, from_status, to_status, source, reason) "
                           "values (?, ?, ?, ?, ?)");
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, task_status_to_text(from)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, task_status_to_text(to)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(4, source); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto b5 = reason.has_value() ? stmt->bind_text(5, *reason) : stmt->bind_null(5);
  if (!b5) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  return {};
}

auto recompute_plan(db::connection& conn, std::int64_t plan_id) -> std::expected<void, task_error> {
  auto result = recompute_status(conn, plan_id);
  if (!result) {
    return std::unexpected(map_plan_error(result.error()));
  }
  return {};
}

auto set_status(db::connection& conn, std::int64_t id, task_status status) -> std::expected<void, task_error> {
  auto stmt = conn.prepare("update tasks set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, task_status_to_text(status)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, id); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  return {};
}

auto is_terminal(task_status s) -> bool {
  return s == task_status::done || s == task_status::cancelled;
}
auto is_open(task_status s) -> bool {
  return s == task_status::todo || s == task_status::doing || s == task_status::blocked;
}

/// @brief Clear `blocked` on every dependent of `blocker_id` whose blocker
/// set is now fully terminal.
///
/// Called after a task reaches a terminal status. A dependent moves
/// `blocked` -> `todo` ONLY when no incomplete blocker remains -- partial
/// clearance (one of three blockers done) is deliberately a no-op, per
/// decision 1122.
///
/// `blocked` is DERIVED state here, not operator intent: it is set by
/// `task block <task> --on <blocker>`, which requires naming a blocker, so
/// the status is defined by the `depends-on` edge. There is no verb that
/// parks a task as `blocked` for reasons unrelated to a dependency, so
/// there is no operator intent for this to override -- leaving the row at
/// `blocked` after its last blocker completes is simply stale.
///
/// Cross-entity auto-transition is not novel here: `recompute_plan` below
/// already transitions a task's parent PLAN from seven call sites in this
/// file.
/// @param conn An open, migrated database connection.
/// @param blocker_id The task that just became terminal.
/// @return Success, or the first query failure.
auto clear_unblocked_dependents(db::connection& conn, std::int64_t blocker_id) -> std::expected<void, task_error> {
  // Dependents still at `blocked` that have NO remaining non-terminal
  // blocker. The `not exists` clause is the all-clear rule: a single
  // outstanding blocker keeps the row blocked.
  auto stmt = conn.prepare("select d.id from tasks d "
                           "join entity_links el on el.from_kind='task' and el.from_id=d.id "
                           "                    and el.to_kind='task' and el.relationship='depends-on' "
                           "where el.to_id = ?1 and d.status = 'blocked' "
                           "  and not exists (select 1 from entity_links other "
                           "                  join tasks b on b.id = other.to_id "
                           "                  where other.from_kind='task' and other.from_id=d.id "
                           "                    and other.to_kind='task' and other.relationship='depends-on' "
                           "                    and b.status not in ('done','cancelled')) "
                           "order by d.id");
  if (!stmt || !stmt->bind_int64(1, blocker_id)) {
    return std::unexpected(task_error::query_failed);
  }
  std::vector<std::int64_t> ready;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(task_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    ready.push_back(stmt->column_int64(0));
  }

  for (auto const dependent : ready) {
    if (auto r = set_status(conn, dependent, task_status::todo); !r) {
      return std::unexpected(r.error());
    }
    // A distinct summary from an operator's own `blocked -> todo`: the two
    // must be distinguishable in the audit trail after the fact.
    if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                       .entity  = {.kind = "task", .id = dependent},
                                                       .summary = std::format("unblocked: task {} is terminal", blocker_id)});
        !a) {
      return std::unexpected(a.error());
    }
  }
  return {};
}

} // namespace

auto task_status_from_text(std::string_view s) -> std::optional<task_status> {
  if (s == "todo") {
    return task_status::todo;
  }
  if (s == "doing") {
    return task_status::doing;
  }
  if (s == "blocked") {
    return task_status::blocked;
  }
  if (s == "done") {
    return task_status::done;
  }
  if (s == "cancelled") {
    return task_status::cancelled;
  }
  return std::nullopt;
}

auto task_status_to_text(task_status s) -> std::string_view {
  switch (s) {
  case task_status::todo:
    return "todo";
  case task_status::doing:
    return "doing";
  case task_status::blocked:
    return "blocked";
  case task_status::done:
    return "done";
  case task_status::cancelled:
    return "cancelled";
  }
  return "todo"; // unreachable
}

auto create_task(db::connection& conn, const task_create_args& args) -> std::expected<task, task_error> {
  auto scope_ref = resolve_scope_or_global(conn, args.scope);
  if (!scope_ref) {
    return std::unexpected(scope_ref.error());
  }

  // AFTER the scope resolution, not before. zig/src/engine/planning/task.zig
  // resolves the scope slug at :309 and only validates `due_at` at :323, so
  // `--scope nosuchscope --due garbage` reports `SlugNotFound`, not
  // `InvalidDueAt`. Both exit 1, so an exit-code test cannot see the
  // difference — only the message can, and the message is the contract.
  if (args.due_at.has_value() && !due_at_is_valid(*args.due_at)) {
    return std::unexpected(task_error::invalid_due_at);
  }

  auto stmt = conn.prepare("insert into tasks (scope_kind, scope_id, plan_id, parent_task_id, title, body, slug, "
                           "status, priority, next_action, due_at) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                           "returning id");
  if (!stmt) {
    return std::unexpected(exec_failed("task.create", "PrepareFailed"));
  }
  if (auto b = stmt->bind_text(1, scope_kind_to_text(scope_ref->first)); !b) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b2 = scope_ref->second.has_value() ? stmt->bind_int64(2, *scope_ref->second) : stmt->bind_null(2);
  if (!b2) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b3 = args.plan_id.has_value() ? stmt->bind_int64(3, *args.plan_id) : stmt->bind_null(3);
  if (!b3) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b4 = args.parent_task_id.has_value() ? stmt->bind_int64(4, *args.parent_task_id) : stmt->bind_null(4);
  if (!b4) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  if (auto b = stmt->bind_text(5, args.title); !b) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b6 = args.body.has_value() ? stmt->bind_text(6, *args.body) : stmt->bind_null(6);
  if (!b6) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b7 = args.slug.has_value() ? stmt->bind_text(7, *args.slug) : stmt->bind_null(7);
  if (!b7) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  if (auto b = stmt->bind_text(8, task_status_to_text(args.status)); !b) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  if (auto b = stmt->bind_int64(9, args.priority); !b) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b10 = args.next_action.has_value() ? stmt->bind_text(10, *args.next_action) : stmt->bind_null(10);
  if (!b10) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }
  auto b11 = args.due_at.has_value() ? stmt->bind_text(11, *args.due_at) : stmt->bind_null(11);
  if (!b11) {
    return std::unexpected(exec_failed("task.create", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    // The unique check precedes the log, exactly as the oracle's
    // `if (d.lastWasUniqueViolation()) return Error.SlugConflict;` precedes
    // its `std.log.err` — a slug conflict emits NO inner line.
    if (is_unique_violation(step.error())) {
      return std::unexpected(task_error::slug_conflict);
    }
    return std::unexpected(exec_failed("task.create", "StepFailed"));
  }
  const auto id = stmt->column_int64(0);

  // ORACLE: `create|task|1|create task 'Task One'` -- the TITLE, and
  // recorded BEFORE the auto-promote recompute below (see `record_audit`).
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "task", .id = id},
                                                     .summary = std::format("create task '{}'", args.title)});
      !a) {
    return std::unexpected(a.error());
  }

  if (!args.no_auto_promote && args.plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *args.plan_id); !r) {
      return std::unexpected(r.error());
    }
  }

  return show_task(conn, id);
}

auto show_task(db::connection& conn, std::int64_t id) -> std::expected<task, task_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(task_error::not_found);
  }
  return read_row(*stmt);
}

auto list_tasks(db::connection& conn, const task_list_filter& filter) -> std::expected<std::vector<task>, task_error> {
  // See `list_plans` for why the set is OR-ed rather than collapsed, and
  // why an unresolvable member fails the call instead of being skipped.
  std::vector<std::pair<task_scope_kind, std::optional<std::int64_t>>> scope_refs;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = resolve_scope_or_global(conn, std::optional<std::string>{s});
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }

  std::string sql(k_select_columns);
  sql += " where 1 = 1";
  if (filter.status.has_value()) {
    sql += " and status = ?";
  } else {
    sql += " and status in ('todo','doing','blocked')";
  }
  if (filter.plan_id.has_value()) {
    sql += " and plan_id = ?";
  }
  if (filter.priority_max.has_value()) {
    sql += " and priority <= ?";
  }
  if (!scope_refs.empty()) {
    sql += " and (";
    for (std::size_t i = 0; i < scope_refs.size(); ++i) {
      if (i > 0) {
        sql += " or ";
      }
      if (scope_refs[i].first == task_scope_kind::global) {
        sql += "scope_kind = 'global'";
      } else {
        sql += "(scope_kind = ? and scope_id = ?)";
      }
    }
    sql += ")";
  }
  sql += " order by priority, updated_at desc, id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  int idx = 1;
  if (filter.status.has_value()) {
    if (auto b = stmt->bind_text(idx++, task_status_to_text(*filter.status)); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.plan_id); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (filter.priority_max.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.priority_max); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }
  for (const auto& ref : scope_refs) {
    if (ref.first == task_scope_kind::global) {
      continue; // literal in the SQL; no placeholder to fill.
    }
    if (!ref.second.has_value()) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto b = stmt->bind_text(idx++, scope_kind_to_text(ref.first)); !b) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *ref.second); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }

  std::vector<task> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(task_error::query_failed);
    }
    if (*step == db::step_result::done) {
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

auto update_task(db::connection& conn, std::int64_t id, const task_update_args& patch) -> std::expected<task, task_error> {
  std::optional<std::pair<task_scope_kind, std::optional<std::int64_t>>> scope_ref;
  if (patch.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, patch.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_ref = *resolved;
  }

  // Same check, same position relative to scope resolution, as
  // `create_task` — zig/src/engine/planning/task.zig validates `patch.due_at`
  // at :731, just after `resolveSlug` at :723.
  if (patch.due_at.has_value() && !due_at_is_valid(*patch.due_at)) {
    return std::unexpected(task_error::invalid_due_at);
  }

  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }

  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  if (patch.status.has_value()) {
    auto checked = check_transition(transition_kind::task, task_status_to_text(current->status),
                                    task_status_to_text(*patch.status), patch.force);
    if (!checked) {
      return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                                 : task_error::illegal_transition);
    }
  }

  enum class param_kind : std::uint8_t { text, int64, null };
  std::string               sql = "update tasks set ";
  std::vector<param_kind>   order;
  std::vector<std::string>  text_params;
  std::vector<std::int64_t> int_params;
  bool                      first = true;
  auto                      sep   = [&] {
    if (!first) {
      sql += ", ";
    }
    first = false;
  };

  if (scope_ref.has_value()) {
    sep();
    sql += "scope_kind = ?";
    order.push_back(param_kind::text);
    text_params.push_back(std::string(scope_kind_to_text(scope_ref->first)));
    sep();
    sql += "scope_id = ?";
    if (scope_ref->second.has_value()) {
      order.push_back(param_kind::int64);
      int_params.push_back(*scope_ref->second);
    } else {
      order.push_back(param_kind::null);
    }
  }
  if (patch.title.has_value()) {
    sep();
    sql += "title = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.title);
  }
  if (patch.body.has_value()) {
    sep();
    sql += "body = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.body);
  }
  if (patch.status.has_value()) {
    sep();
    sql += "status = ?";
    order.push_back(param_kind::text);
    text_params.push_back(std::string(task_status_to_text(*patch.status)));
  }
  if (patch.priority.has_value()) {
    sep();
    sql += "priority = ?";
    order.push_back(param_kind::int64);
    int_params.push_back(*patch.priority);
  }
  if (patch.plan_id.has_value()) {
    sep();
    sql += "plan_id = ?";
    order.push_back(param_kind::int64);
    int_params.push_back(*patch.plan_id);
  } else if (patch.clear_plan) {
    sep();
    sql += "plan_id = null";
  }
  if (patch.next_action.has_value()) {
    sep();
    sql += "next_action = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.next_action);
  }
  if (patch.due_at.has_value()) {
    sep();
    sql += "due_at = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.due_at);
  }
  if (patch.slug.has_value()) {
    sep();
    sql += "slug = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.slug);
  }

  if (first) {
    // Before the audit write below on purpose: a patch that changes
    // nothing writes no `audit_log` row in the oracle either.
    return show_task(conn, id);
  }

  sep();
  sql += "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(exec_failed("task.update", "PrepareFailed"));
  }
  int         bind_idx = 1;
  std::size_t text_i   = 0;
  std::size_t int_i    = 0;
  for (const auto k : order) {
    switch (k) {
    case param_kind::text:
      if (auto b = stmt->bind_text(bind_idx++, text_params[text_i++]); !b) {
        return std::unexpected(exec_failed("task.update", "BindFailed"));
      }
      break;
    case param_kind::int64:
      if (auto b = stmt->bind_int64(bind_idx++, int_params[int_i++]); !b) {
        return std::unexpected(exec_failed("task.update", "BindFailed"));
      }
      break;
    case param_kind::null:
      // Every `?` placeholder in `sql` -- including a NULL scope_id --
      // consumes one positional bind slot. Skipping the increment here
      // shifted every subsequent bind (title, body, status, ...) left by
      // one, silently mis-binding them (and leaving the real update a
      // no-op in the common `--scope global` case, task 6453).
      if (auto b = stmt->bind_null(bind_idx++); !b) {
        return std::unexpected(exec_failed("task.update", "BindFailed"));
      }
      break;
    }
  }
  if (auto b = stmt->bind_int64(bind_idx++, id); !b) {
    return std::unexpected(exec_failed("task.update", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(task_error::slug_conflict);
    }
    return std::unexpected(exec_failed("task.update", "StepFailed"));
  }

  // ORACLE: a patch carrying a status writes `status_change`, any other
  // patch writes `update`, and BOTH carry a NULL summary -- captured as
  // `status_change|task|3|<NULL>` from `task update 3 --status todo
  // --force` and `update|task|3|<NULL>` from `task update 3 --title ...`.
  // Note this is the ONE status_change on a task with no summary text;
  // `done`/`cancel`/`block`/`reopen` all carry one.
  if (auto a = record_audit(
          conn, audit::record_args{.verb   = patch.status.has_value() ? audit::verb::status_change : audit::verb::update,
                                   .entity = {.kind = "task", .id = id}});
      !a) {
    return std::unexpected(a.error());
  }

  if (patch.force && patch.status.has_value()) {
    if (is_terminal(current->status) && is_open(*patch.status)) {
      if (auto r = record_reopen(conn, id, current->status, *patch.status, "task-update-force", patch.reason); !r) {
        return std::unexpected(r.error());
      }
    }
  }

  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }

  // `task update --status done|cancelled` reaches a terminal status without
  // going through `mark_done`/`mark_cancelled` -- it writes `status` in its
  // own UPDATE above -- so the dependent sweep has to be wired here too, or
  // the auto-clear would fire on two of the three terminal paths and look
  // arbitrary (decision 1122).
  if (patch.status.has_value() && is_terminal(*patch.status) && !is_terminal(current->status)) {
    if (auto r = clear_unblocked_dependents(conn, id); !r) {
      return std::unexpected(r.error());
    }
  }

  if (!patch.no_auto_promote) {
    if (current->plan_id.has_value()) {
      if (auto r = recompute_plan(conn, *current->plan_id); !r) {
        return std::unexpected(r.error());
      }
    }
    if (updated->plan_id.has_value() && (!current->plan_id.has_value() || *current->plan_id != *updated->plan_id)) {
      if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
        return std::unexpected(r.error());
      }
    }
  }

  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto mark_done(db::connection& conn, std::int64_t id, bool force) -> std::expected<task, task_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto checked = check_transition(transition_kind::task, task_status_to_text(current->status), "done", force);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, task_status::done); !r) {
    return std::unexpected(r.error());
  }
  // Decision 1122: a dependent whose LAST blocker just became terminal is
  // no longer blocked, and leaving it at `blocked` is stale state.
  if (auto r = clear_unblocked_dependents(conn, id); !r) {
    return std::unexpected(r.error());
  }
  // ORACLE: the summary is the literal `done` -- the resulting status
  // word, not a sentence.
  if (auto a = record_audit(
          conn, audit::record_args{.verb = audit::verb::status_change, .entity = {.kind = "task", .id = id}, .summary = "done"});
      !a) {
    return std::unexpected(a.error());
  }
  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto mark_cancelled(db::connection& conn, std::int64_t id) -> std::expected<task, task_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto checked = check_transition(transition_kind::task, task_status_to_text(current->status), "cancelled", false);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, task_status::cancelled); !r) {
    return std::unexpected(r.error());
  }
  // Cancelled is terminal and does NOT block (docs/concepts.md's closeout
  // gate says so explicitly), so it clears dependents exactly as `done`
  // does -- decision 1122.
  if (auto r = clear_unblocked_dependents(conn, id); !r) {
    return std::unexpected(r.error());
  }
  // ORACLE: `cancelled` -- the STATUS word, so it does not match the verb
  // name `cancel` the operator typed.
  if (auto a = record_audit(
          conn,
          audit::record_args{.verb = audit::verb::status_change, .entity = {.kind = "task", .id = id}, .summary = "cancelled"});
      !a) {
    return std::unexpected(a.error());
  }
  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto mark_blocked(db::connection& conn, std::int64_t id, std::int64_t blocked_on_id, std::optional<std::string_view> reason,
                  bool force) -> std::expected<task, task_error> {
  // Verify the blocker exists (mirrors zig: read-only, before the transaction).
  auto blocker = show_task(conn, blocked_on_id);
  if (!blocker) {
    return std::unexpected(blocker.error());
  }

  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto checked = check_transition(transition_kind::task, task_status_to_text(current->status), "blocked", force);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, task_status::blocked); !r) {
    return std::unexpected(r.error());
  }

  // ORACLE: `blocked on task 2: waiting on T2` with a reason, and
  // `blocked on task 1` -- no trailing colon -- without one. The
  // `entity_links` row inserted below produces NO audit row of its own.
  if (auto a = record_audit(
          conn, audit::record_args{.verb    = audit::verb::status_change,
                                   .entity  = {.kind = "task", .id = id},
                                   .summary = reason.has_value() ? std::format("blocked on task {}: {}", blocked_on_id, *reason)
                                                                 : std::format("blocked on task {}", blocked_on_id)});
      !a) {
    return std::unexpected(a.error());
  }

  {
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                             "values ('task', ?, 'task', ?, 'depends-on')");
    if (!stmt) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, blocked_on_id); !b) {
      return std::unexpected(task_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(task_error::query_failed);
    }
  }

  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto reopen(db::connection& conn, std::int64_t id, task_status new_status, std::string_view reason)
    -> std::expected<task, task_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  // `reopen` is the verb-gated escape from terminal status — bypasses the
  // matrix (force=true), mirrors zig task.zig:1149.
  auto checked =
      check_transition(transition_kind::task, task_status_to_text(current->status), task_status_to_text(new_status), true);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, new_status); !r) {
    return std::unexpected(r.error());
  }

  // ORACLE: `reopen to todo: regressed`. The reason is not optional on
  // this path -- the leaf refuses with `--reason is required for reopen`
  // before the engine is reached -- so there is no reason-less arm to
  // mirror here.
  if (auto a = record_audit(
          conn, audit::record_args{.verb    = audit::verb::status_change,
                                   .entity  = {.kind = "task", .id = id},
                                   .summary = std::format("reopen to {}: {}", task_status_to_text(new_status), reason)});
      !a) {
    return std::unexpected(a.error());
  }

  if (is_terminal(current->status) && is_open(new_status)) {
    if (auto r = record_reopen(conn, id, current->status, new_status, "task-reopen", reason); !r) {
      return std::unexpected(r.error());
    }
  }

  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto list_tasks_touching(db::connection& conn, std::int64_t repo_id, const task_list_filter& filter)
    -> std::expected<std::vector<task>, task_error> {
  std::vector<std::pair<task_scope_kind, std::optional<std::int64_t>>> scope_refs;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = resolve_scope_or_global(conn, std::optional<std::string>{s});
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }

  // All-or-nothing on the direct branch; see `list_plans_touching`.
  bool branch_direct = true;
  if (!scope_refs.empty()) {
    branch_direct = false;
    for (const auto& ref : scope_refs) {
      if (ref.first == task_scope_kind::repo && (!ref.second.has_value() || *ref.second == repo_id)) {
        branch_direct = true;
        break;
      }
    }
  }

  auto const status_clause =
      filter.status.has_value() ? std::string{" and status = ?"} : std::string{" and status in ('todo','doing','blocked')"};

  std::string sql = "select * from (";
  sql += k_select_columns;
  sql += " where 1 = 1";
  if (branch_direct) {
    sql += " and scope_kind = 'repo' and scope_id = ?";
    sql += status_clause;
    if (filter.plan_id.has_value()) {
      sql += " and plan_id = ?";
    }
    if (filter.priority_max.has_value()) {
      sql += " and priority <= ?";
    }
  } else {
    sql += " and 1 = 0";
  }
  sql += " union ";
  sql += k_select_columns;
  sql += " where 1 = 1";
  sql += " and id in (select from_id from entity_links where from_kind = 'task' and to_kind = 'repo' and to_id = ? and "
         "relationship = 'touches')";
  sql += status_clause;
  if (!scope_refs.empty()) {
    sql += " and (";
    for (std::size_t i = 0; i < scope_refs.size(); ++i) {
      if (i > 0) {
        sql += " or ";
      }
      if (scope_refs[i].first == task_scope_kind::global) {
        sql += "scope_kind = 'global'";
      } else {
        sql += "(scope_kind = ? and scope_id = ?)";
      }
    }
    sql += ")";
  }
  if (filter.plan_id.has_value()) {
    sql += " and plan_id = ?";
  }
  if (filter.priority_max.has_value()) {
    sql += " and priority <= ?";
  }
  // `order by id` — NOT `list_tasks`' `priority, updated_at desc, id`. The
  // oracle wraps the UNION and orders by id alone; see the header.
  sql += ") order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  int  idx       = 1;
  auto bind_int  = [&](std::int64_t v) { return stmt->bind_int64(idx++, v).has_value(); };
  auto bind_text = [&](std::string_view v) { return stmt->bind_text(idx++, v).has_value(); };

  if (branch_direct) {
    if (!bind_int(repo_id)) {
      return std::unexpected(task_error::query_failed);
    }
    if (filter.status.has_value() && !bind_text(task_status_to_text(*filter.status))) {
      return std::unexpected(task_error::query_failed);
    }
    if (filter.plan_id.has_value() && !bind_int(*filter.plan_id)) {
      return std::unexpected(task_error::query_failed);
    }
    if (filter.priority_max.has_value() && !bind_int(*filter.priority_max)) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (!bind_int(repo_id)) {
    return std::unexpected(task_error::query_failed);
  }
  if (filter.status.has_value() && !bind_text(task_status_to_text(*filter.status))) {
    return std::unexpected(task_error::query_failed);
  }
  for (const auto& ref : scope_refs) {
    if (ref.first == task_scope_kind::global) {
      continue;
    }
    if (!ref.second.has_value()) {
      return std::unexpected(task_error::query_failed);
    }
    if (!bind_text(scope_kind_to_text(ref.first)) || !bind_int(*ref.second)) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (filter.plan_id.has_value() && !bind_int(*filter.plan_id)) {
    return std::unexpected(task_error::query_failed);
  }
  if (filter.priority_max.has_value() && !bind_int(*filter.priority_max)) {
    return std::unexpected(task_error::query_failed);
  }

  std::vector<task> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(task_error::query_failed);
    }
    if (*step == db::step_result::done) {
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

auto render_text(const task& t) -> std::string {
  std::string out;
  out += std::format("id:          {}\n", t.id);
  out += std::format("title:       {}\n", t.title);
  out += std::format("status:      {}\n", task_status_to_text(t.status));
  // CLAMPED, unlike the JSON renderer. See the `@return` block on this
  // function's declaration: the oracle prints `@max(priority, 0)` while
  // the column keeps the signed value.
  out += std::format("priority:    {}\n", std::max<std::int64_t>(t.priority, 0));
  out += std::format("scope:       {}", scope_kind_to_text(t.scope_kind));
  if (t.scope_id.has_value()) {
    out += std::format(":{}", *t.scope_id);
  }
  out += "\n";
  if (t.plan_id.has_value()) {
    out += std::format("plan:        {}\n", *t.plan_id);
  }
  if (t.parent_task_id.has_value()) {
    out += std::format("parent:      {}\n", *t.parent_task_id);
  }
  if (t.next_action.has_value()) {
    out += std::format("next action: {}\n", *t.next_action);
  }
  if (t.due_at.has_value()) {
    out += std::format("due:         {}\n", *t.due_at);
  }
  if (t.body.has_value()) {
    out += std::format("body:        {}\n", *t.body);
  }
  out += std::format("created:     {}\n", t.created_at);
  out += std::format("updated:     {}\n", t.updated_at);
  return out;
}

auto render_json(const task& t) -> std::string {
  auto const opt_int = [](std::optional<std::int64_t> v) -> std::string {
    return v.has_value() ? std::format("{}", *v) : std::string{"null"};
  };
  auto const opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  return std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},"plan_id":{},"parent_task_id":{},"title":{},)"
                     R"("body":{},"slug":{},"status":"{}","priority":{},"next_action":{},"due_at":{},)"
                     R"("created_at":{},"updated_at":{}}})",
                     t.id, scope_kind_to_text(t.scope_kind), opt_int(t.scope_id), opt_int(t.plan_id), opt_int(t.parent_task_id),
                     json_string(t.title), opt_str(t.body), opt_str(t.slug), task_status_to_text(t.status), t.priority,
                     opt_str(t.next_action), opt_str(t.due_at), json_string(t.created_at), json_string(t.updated_at));
}

auto render_list_text(std::span<const task> tasks) -> std::string {
  if (tasks.empty()) {
    return "(no tasks)\n";
  }
  std::string out;
  for (const auto& t : tasks) {
    out += std::format("{:>5}  pri {:>3}  {:<10}  {}\n", t.id, std::max<std::int64_t>(t.priority, 0),
                       task_status_to_text(t.status), t.title);
  }
  return out;
}

auto render_list_json(std::span<const task> tasks) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(tasks[i]);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
