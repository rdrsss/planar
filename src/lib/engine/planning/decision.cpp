/// @file decision.cpp
/// @brief Implementation of `planar.engine.planning.decision` (see
/// decision.cppm).

module;

module planar.engine.planning.decision;

import std;
import planar.db;
import planar.json_text;
import planar.log;
import planar.scope_ref;
import planar.policy;
import planar.engine.planning.transitions;

namespace planar::engine::planning {

using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>`
/// diagnostic ahead of the outer handler error. See
/// `zig/src/engine/planning/decision.zig`'s `create` for the shape this
/// ports; mirrors `exec_failed` in task.cpp (same bucket, separate TU).
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> decision_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return decision_error::query_failed;
}

/// @brief SQLITE_CONSTRAINT_UNIQUE — the same constant
/// `engine_entitylink`'s `entitylink.cpp` and `engine_planning`'s
/// `plan.cpp` already name, repeated rather than shared because a
/// three-line predicate over a documented SQLite constant is vocabulary,
/// not policy.
constexpr int k_sqlite_constraint_unique = 2067;

/// @brief Whether `err` is the UNIQUE-constraint failure.
/// @param err The db failure to classify.
/// @return True for `SQLITE_CONSTRAINT_UNIQUE`.
auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface. Called AFTER the decision's own write succeeds —
/// a refused decision mutation writes no audit row in the oracle (verified:
/// a `supersede` refused as `LinkExists` leaves `audit_log` untouched).
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, decision_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(decision_error::audit_write_failed);
  }
  return {};
}

/// @brief Render a scope kind as its column text.
/// @param k The scope kind.
/// @return The column text.
auto scope_kind_to_text(decision_scope_kind k) -> std::string_view {
  switch (k) {
  case decision_scope_kind::global:
    return "global";
  case decision_scope_kind::association:
    return "association";
  case decision_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` onto this module's own
/// `decision_scope_kind`. A 1:1 mapping kept explicit, matching `plan.cpp`.
/// @param k The layer-1 scope kind.
/// @return This module's corresponding scope kind.
auto to_decision_kind(scope_ref::scope_kind k) -> decision_scope_kind {
  switch (k) {
  case scope_ref::scope_kind::global:
    return decision_scope_kind::global;
  case scope_ref::scope_kind::association:
    return decision_scope_kind::association;
  case scope_ref::scope_kind::repo:
    return decision_scope_kind::repo;
  }
  return decision_scope_kind::global; // unreachable
}

/// @brief Map a `planar.scope_ref` failure onto this module's error surface.
/// @param e The layer-1 failure.
/// @return The corresponding `decision_error`.
auto to_decision_error(scope_ref::error e) -> decision_error {
  switch (e) {
  case scope_ref::error::query_failed:
    return decision_error::query_failed;
  case scope_ref::error::slug_not_found:
    return decision_error::slug_not_found;
  }
  return decision_error::query_failed; // unreachable
}

/// @brief A resolved scope: the kind plus the row id (unset for global).
using resolved_scope = std::pair<decision_scope_kind, std::optional<std::int64_t>>;

/// @brief Resolve `scope` (when present) to a `(kind,id)` pair, else global.
///
/// Delegates the `"global"` / `"repo:<slug>"` / `"assoc:<slug>"` /
/// bare-association grammar to layer-1 `planar.scope_ref::resolve` (D19).
/// Only the "no scope argument at all -> global" fold — distinct from the
/// literal string `"global"` — stays local.
/// @param conn An open, migrated connection.
/// @param scope The scope slug, or unset.
/// @return The resolved pair, or the mapped failure.
auto resolve_scope_or_global(db::connection& conn, const std::optional<std::string>& scope)
    -> std::expected<resolved_scope, decision_error> {
  if (!scope.has_value()) {
    return std::make_pair(decision_scope_kind::global, std::optional<std::int64_t>{});
  }
  auto resolved = scope_ref::resolve(conn, *scope);
  if (!resolved) {
    return std::unexpected(to_decision_error(resolved.error()));
  }
  return std::make_pair(to_decision_kind(resolved->kind), resolved->id);
}

/// @brief Resolve `filter.scope` followed by every member of `filter.scopes`
/// into ONE ordered disjunction.
///
/// An unresolvable slug ANYWHERE in the set fails the whole call rather
/// than being skipped: a read verb that quietly drops one member of its
/// scope set returns a short list that looks complete.
/// @param conn An open, migrated connection.
/// @param filter The filter carrying the scope members.
/// @return The resolved refs in order, or the first failure.
auto resolve_scope_set(db::connection& conn, const decision_list_filter& filter)
    -> std::expected<std::vector<resolved_scope>, decision_error> {
  std::vector<resolved_scope> refs;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    refs.push_back(*resolved);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = resolve_scope_or_global(conn, std::optional<std::string>{s});
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    refs.push_back(*resolved);
  }
  return refs;
}

constexpr std::string_view k_select_columns = "select id, scope_kind, scope_id, title, body, rationale, status, "
                                              "decided_at, session_id, created_at, updated_at from decisions";

/// @brief Decode one row of `k_select_columns` into a `decision`.
/// @param stmt A statement positioned on a row.
/// @return The decoded row, or `query_failed` for an unparseable enum
/// column.
auto read_row(db::statement& stmt) -> std::expected<decision, decision_error> {
  const auto          scope_kind_text = stmt.column_text(1);
  decision_scope_kind sk{};
  if (scope_kind_text == "global") {
    sk = decision_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = decision_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = decision_scope_kind::repo;
  } else {
    return std::unexpected(decision_error::query_failed);
  }

  const auto status = decision_status_from_text(stmt.column_text(6));
  if (!status.has_value()) {
    return std::unexpected(decision_error::query_failed);
  }

  return decision{
      .id         = stmt.column_int64(0),
      .scope_kind = sk,
      .scope_id   = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .title      = stmt.column_text(3),
      // `body` is `not null`; it is read unconditionally and an empty
      // column yields `""`, never a disguised null.
      .body       = stmt.column_text(4),
      .rationale  = stmt.is_null(5) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(5)},
      .status     = *status,
      .decided_at = stmt.is_null(7) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(7)},
      .session_id = stmt.is_null(8) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(8)},
      .created_at = stmt.column_text(9),
      .updated_at = stmt.column_text(10),
  };
}

/// @brief Read every remaining row of `stmt` into a vector.
/// @param stmt A bound, un-stepped statement over `k_select_columns`.
/// @return The rows in statement order, or the first decode/step failure.
auto collect_rows(db::statement& stmt) -> std::expected<std::vector<decision>, decision_error> {
  std::vector<decision> out;
  for (;;) {
    auto step = stmt.step();
    if (!step) {
      return std::unexpected(decision_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto row = read_row(stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  return out;
}

/// @brief Validate a decision status move, mapping the shared matrix's
/// vocabulary back onto this module's two spellings.
///
/// Mirrors zig's `decision.validateTransition`: an illegal move from a
/// TERMINAL source is `terminal_status`, an illegal move from any other
/// source is `invalid_status`, and an unrecognized source is
/// `invalid_status` too. The distinction is operator-visible — `decision
/// accept <withdrawn>` says "is terminal; cannot accept" and nothing else
/// in the family produces that wording.
/// @param current The row's current status.
/// @param next The status being moved to.
/// @return Success, or the mapped refusal.
auto validate_transition(decision_status current, decision_status next) -> std::expected<void, decision_error> {
  auto checked =
      check_transition(transition_kind::decision, decision_status_to_text(current), decision_status_to_text(next), false);
  if (checked) {
    return {};
  }
  if (checked.error() == transition_error::illegal_transition && decision_status_is_terminal(current)) {
    return std::unexpected(decision_error::terminal_status);
  }
  return std::unexpected(decision_error::invalid_status);
}

/// @brief Shared body of `accept_decision` / `withdraw_decision`.
///
/// Both read the current row, validate the transition, run their own
/// UPDATE, write one `status_change` audit row and re-read — inside one
/// transaction so a failed audit write takes the status flip with it. The
/// only differences are whether `decided_at` is re-stamped and what the
/// summary says, which the caller supplies.
/// @param conn An open, migrated connection.
/// @param id The decision's row id.
/// @param target The status being moved to.
/// @param set_decided_at Whether to re-stamp `decided_at` in the same
/// statement (true for `accept` only).
/// @param summary The `audit_log` summary to record — the BARE verb label
/// (`accept` / `withdraw`), with no id and no colon.
/// @return The updated row, or the first failure.
auto transition(db::connection& conn, std::int64_t id, decision_status target, bool set_decided_at, std::string_view summary)
    -> std::expected<decision, decision_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(decision_error::query_failed);
  }

  auto current = show_decision(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (auto checked = validate_transition(current->status, target); !checked) {
    return std::unexpected(checked.error());
  }

  {
    // `decided_at` is stamped ONLY on the accept path. `withdraw` and
    // `supersede` leave the column alone, so an accepted-then-withdrawn
    // decision keeps its stamp — visible as a `decided:` line on a
    // `withdrawn` row.
    const std::string_view sql =
        set_decided_at ? "update decisions set status = ?, decided_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
                         "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?"
                       : "update decisions set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?";
    auto stmt = conn.prepare(sql);
    if (!stmt) {
      return std::unexpected(decision_error::query_failed);
    }
    if (auto b = stmt->bind_text(1, decision_status_to_text(target)); !b) {
      return std::unexpected(decision_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, id); !b) {
      return std::unexpected(decision_error::query_failed);
    }
    if (!stmt->step().has_value()) {
      return std::unexpected(decision_error::query_failed);
    }
  }

  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "decision", .id = id},
                                                     .summary = std::string{summary}});
      !a) {
    return std::unexpected(a.error());
  }

  auto updated = show_decision(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(decision_error::query_failed);
  }
  return updated;
}

} // namespace

auto decision_status_from_text(std::string_view s) -> std::optional<decision_status> {
  if (s == "proposed") {
    return decision_status::proposed;
  }
  if (s == "accepted") {
    return decision_status::accepted;
  }
  if (s == "superseded") {
    return decision_status::superseded;
  }
  if (s == "withdrawn") {
    return decision_status::withdrawn;
  }
  return std::nullopt;
}

auto decision_status_to_text(decision_status s) -> std::string_view {
  switch (s) {
  case decision_status::proposed:
    return "proposed";
  case decision_status::accepted:
    return "accepted";
  case decision_status::superseded:
    return "superseded";
  case decision_status::withdrawn:
    return "withdrawn";
  }
  return "proposed"; // unreachable
}

auto decision_status_is_terminal(decision_status s) -> bool {
  return s == decision_status::superseded || s == decision_status::withdrawn;
}

auto create_decision(db::connection& conn, const decision_create_args& args) -> std::expected<decision, decision_error> {
  auto scope = resolve_scope_or_global(conn, args.scope);
  if (!scope) {
    return std::unexpected(scope.error());
  }

  // NOTE the absent `--plan` existence check. `question add --plan 9999`
  // refuses before writing anything; `decision add --plan 9999` succeeds
  // and leaves a dangling edge. Oracle-captured, reproduced (D2) — see
  // `decision_create_args::plan_id`.

  std::int64_t id = 0;
  {
    auto stmt = conn.prepare("insert into decisions (scope_kind, scope_id, title, body, rationale, status, session_id) "
                             "values (?, ?, ?, ?, ?, 'proposed', ?) returning id");
    if (!stmt) {
      return std::unexpected(exec_failed("decision.create", "PrepareFailed"));
    }
    if (auto b = stmt->bind_text(1, scope_kind_to_text(scope->first)); !b) {
      return std::unexpected(exec_failed("decision.create", "BindFailed"));
    }
    auto b2 = scope->second.has_value() ? stmt->bind_int64(2, *scope->second) : stmt->bind_null(2);
    if (!b2) {
      return std::unexpected(exec_failed("decision.create", "BindFailed"));
    }
    if (auto b = stmt->bind_text(3, args.title); !b) {
      return std::unexpected(exec_failed("decision.create", "BindFailed"));
    }
    // `body` is `not null` and is bound unconditionally — an empty body
    // binds `''`, never NULL.
    if (auto b = stmt->bind_text(4, args.body); !b) {
      return std::unexpected(exec_failed("decision.create", "BindFailed"));
    }
    // `rationale` binds SQL NULL when absent, NOT the empty string:
    // `render_json` emits `null` vs `""` and the two are operator-visible.
    auto b5 = args.rationale.has_value() ? stmt->bind_text(5, *args.rationale) : stmt->bind_null(5);
    if (!b5) {
      return std::unexpected(exec_failed("decision.create", "BindFailed"));
    }
    auto b6 = args.session_id.has_value() ? stmt->bind_int64(6, *args.session_id) : stmt->bind_null(6);
    if (!b6) {
      return std::unexpected(exec_failed("decision.create", "BindFailed"));
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(exec_failed("decision.create", "StepFailed"));
    }
    if (*step != db::step_result::row) {
      return std::unexpected(exec_failed("decision.create", "StepFailed"));
    }
    id = stmt->column_int64(0);
  }

  // ORACLE: `create|decision|1|create decision 'first'` — the TITLE in
  // single quotes, and the noun is `decision`, not `create decision:`.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "decision", .id = id},
                                                     .summary = std::format("create decision '{}'", args.title)});
      !a) {
    return std::unexpected(a.error());
  }

  if (args.plan_id.has_value()) {
    // No `link` audit row here — the oracle writes exactly one row for
    // `decision add --plan`, and its verb is `create`.
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                             "values ('decision', ?, 'plan', ?, 'derives-from')");
    if (!stmt) {
      return std::unexpected(decision_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return std::unexpected(decision_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, *args.plan_id); !b) {
      return std::unexpected(decision_error::query_failed);
    }
    if (!stmt->step().has_value()) {
      return std::unexpected(decision_error::query_failed);
    }
  }

  return show_decision(conn, id);
}

auto show_decision(db::connection& conn, std::int64_t id) -> std::expected<decision, decision_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(decision_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(decision_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(decision_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(decision_error::not_found);
  }
  return read_row(*stmt);
}

auto list_decisions(db::connection& conn, const decision_list_filter& filter)
    -> std::expected<std::vector<decision>, decision_error> {
  auto refs = resolve_scope_set(conn, filter);
  if (!refs) {
    return std::unexpected(refs.error());
  }

  std::string sql = std::string(k_select_columns) + " where 1 = 1";
  if (filter.status.has_value()) {
    sql += " and status = ?";
  } else {
    // THE DEFAULT IS A PREDICATE, NOT ITS ABSENCE. Unset means the two
    // OPEN lifecycle states; there is no "list everything" value.
    sql += " and status in ('proposed','accepted')";
  }
  if (!refs->empty()) {
    sql += " and (";
    for (std::size_t i = 0; i < refs->size(); ++i) {
      if (i > 0) {
        sql += " or ";
      }
      if ((*refs)[i].first == decision_scope_kind::global) {
        // `global` matches on `scope_kind` ALONE — the oracle emits a bare
        // `scope_kind = 'global'` with no `scope_id is null` conjunct.
        sql += "scope_kind = 'global'";
      } else if ((*refs)[i].first == decision_scope_kind::association) {
        sql += "(scope_kind = 'association' and scope_id = ?)";
      } else {
        sql += "(scope_kind = 'repo' and scope_id = ?)";
      }
    }
    sql += ")";
  }
  if (filter.plan_id.has_value()) {
    sql += " and id in (select from_id from entity_links where from_kind='decision' and to_kind='plan' and "
           "relationship='derives-from' and to_id=?)";
  }
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(decision_error::query_failed);
  }
  int idx = 1;
  if (filter.status.has_value()) {
    if (auto b = stmt->bind_text(idx++, decision_status_to_text(*filter.status)); !b) {
      return std::unexpected(decision_error::query_failed);
    }
  }
  for (const auto& ref : *refs) {
    if (ref.first == decision_scope_kind::global) {
      continue; // literal in the SQL; no placeholder to fill.
    }
    if (!ref.second.has_value()) {
      // A non-global ref with no row id cannot become a predicate. The
      // oracle unwraps the optional unconditionally and would panic;
      // refuse rather than bind a placeholder id, which would silently
      // match nothing and return a short list.
      return std::unexpected(decision_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *ref.second); !b) {
      return std::unexpected(decision_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.plan_id); !b) {
      return std::unexpected(decision_error::query_failed);
    }
  }

  return collect_rows(*stmt);
}

auto accept_decision(db::connection& conn, std::int64_t id) -> std::expected<decision, decision_error> {
  // ORACLE: the audit summary is the bare word `accept`.
  return transition(conn, id, decision_status::accepted, true, "accept");
}

auto withdraw_decision(db::connection& conn, std::int64_t id) -> std::expected<decision, decision_error> {
  // ORACLE: the audit summary is the bare word `withdraw`.
  return transition(conn, id, decision_status::withdrawn, false, "withdraw");
}

auto supersede_decision(db::connection& conn, std::int64_t old_id, std::int64_t new_id)
    -> std::expected<decision, decision_error> {
  auto current = show_decision(conn, old_id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (auto checked = validate_transition(current->status, decision_status::superseded); !checked) {
    return std::unexpected(checked.error());
  }
  // The NEW decision must exist, and the check happens BEFORE any write —
  // `decision supersede 3 --by 999` leaves decision 3 untouched. Its own
  // STATUS is never consulted: superseding by a withdrawn decision is
  // permitted.
  if (auto new_row = show_decision(conn, new_id); !new_row) {
    return std::unexpected(new_row.error());
  }

  {
    // The status flip and the edge share ONE transaction; the rollback is
    // observable (see the header comment on this function).
    auto tx = conn.begin_transaction();
    if (!tx) {
      return std::unexpected(decision_error::query_failed);
    }

    {
      auto stmt = conn.prepare("update decisions set status = 'superseded', "
                               "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
      if (!stmt) {
        return std::unexpected(decision_error::query_failed);
      }
      if (auto b = stmt->bind_int64(1, old_id); !b) {
        return std::unexpected(decision_error::query_failed);
      }
      if (!stmt->step().has_value()) {
        return std::unexpected(decision_error::query_failed);
      }
    }

    {
      // Written directly, NOT through `planar.engine.entitylink::add` — see
      // decision.cppm's header for the two independent reasons.
      auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                               "values ('decision', ?, 'decision', ?, 'supersedes')");
      if (!stmt) {
        return std::unexpected(decision_error::query_failed);
      }
      if (auto b = stmt->bind_int64(1, new_id); !b) {
        return std::unexpected(decision_error::query_failed);
      }
      if (auto b = stmt->bind_int64(2, old_id); !b) {
        return std::unexpected(decision_error::query_failed);
      }
      auto step = stmt->step();
      if (!step) {
        // The transaction is NOT committed, so the destructor rolls the
        // status UPDATE back on the way out of this scope. That rollback is
        // the whole point of the transaction and is asserted directly.
        if (is_unique_violation(step.error())) {
          return std::unexpected(decision_error::link_exists);
        }
        return std::unexpected(decision_error::query_failed);
      }
    }

    if (auto committed = tx->commit(); !committed) {
      return std::unexpected(decision_error::query_failed);
    }
  }

  // The audit row lands AFTER the transaction commits, matching the
  // oracle's savepoint boundary: a failed audit write leaves the status
  // flip and the edge standing rather than reverting them.
  if (auto a = record_audit(conn,
                            audit::record_args{
                                .verb    = audit::verb::status_change,
                                .entity  = {.kind = "decision", .id = old_id},
                                .summary = std::format("supersede: decision {} superseded by decision {}", old_id, new_id),
                            });
      !a) {
    return std::unexpected(a.error());
  }

  return show_decision(conn, old_id);
}

auto decisions_for_task(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<decision>, decision_error> {
  auto stmt = conn.prepare("select d.id, d.scope_kind, d.scope_id, d.title, d.body, d.rationale, d.status, "
                           "d.decided_at, d.session_id, d.created_at, d.updated_at "
                           "from decisions d left join sessions s on s.id = d.session_id "
                           "where s.task_id = ? or d.scope_kind = 'global' "
                           "group by d.id order by d.id");
  if (!stmt) {
    return std::unexpected(decision_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(decision_error::query_failed);
  }
  return collect_rows(*stmt);
}

auto render_text(const decision& d) -> std::string {
  std::string out;
  out += std::format("id:         {}\n", d.id);
  out += std::format("title:      {}\n", d.title);
  out += std::format("status:     {}\n", decision_status_to_text(d.status));
  out += std::format("scope:      {}", scope_kind_to_text(d.scope_kind));
  if (d.scope_id.has_value()) {
    out += std::format(":{}", *d.scope_id);
  }
  out += "\n";
  out += std::format("body:       {}\n", d.body);
  if (d.rationale.has_value()) {
    out += std::format("rationale:  {}\n", *d.rationale);
  }
  if (d.decided_at.has_value()) {
    out += std::format("decided:    {}\n", *d.decided_at);
  }
  if (d.session_id.has_value()) {
    out += std::format("session:    {}\n", *d.session_id);
  }
  out += std::format("created:    {}\n", d.created_at);
  out += std::format("updated:    {}\n", d.updated_at);
  return out;
}

auto render_json(const decision& d) -> std::string {
  auto const opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  auto const opt_int = [](const std::optional<std::int64_t>& v) -> std::string {
    return v.has_value() ? std::format("{}", *v) : std::string{"null"};
  };
  return std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},"title":{},"body":{},"rationale":{},"status":"{}",)"
                     R"("decided_at":{},"session_id":{},"created_at":{},"updated_at":{}}})",
                     d.id, scope_kind_to_text(d.scope_kind), opt_int(d.scope_id), json_string(d.title), json_string(d.body),
                     opt_str(d.rationale), decision_status_to_text(d.status), opt_str(d.decided_at), opt_int(d.session_id),
                     json_string(d.created_at), json_string(d.updated_at));
}

auto render_list_text(std::span<const decision> items) -> std::string {
  if (items.empty()) {
    // NO PARENTHESES. `question` renders `(no questions)`; this one does
    // not, and the difference was captured, not assumed.
    return "no decisions\n";
  }
  std::string out;
  for (const auto& d : items) {
    out += std::format("{:>5}  {:<10}  {}\n", d.id, decision_status_to_text(d.status), d.title);
  }
  return out;
}

auto render_list_json(std::span<const decision> items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(items[i]);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
