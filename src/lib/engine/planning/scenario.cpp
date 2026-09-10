/// @file scenario.cpp
/// @brief Implementation of `planar.engine.planning.scenario` (see
/// scenario.cppm).

module;

module planar.engine.planning.scenario;

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
/// `zig/src/engine/planning/scenario.zig`'s `create` for the shape this
/// ports; mirrors `exec_failed` in task.cpp (same bucket, separate TU).
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> scenario_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return scenario_error::query_failed;
}

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface. Called AFTER the scenario's own write succeeds —
/// a refused scenario mutation writes no audit row in the oracle (verified:
/// `scenario verify <retired>` with the default `pass` leaves `audit_log`
/// untouched).
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, scenario_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(scenario_error::audit_write_failed);
  }
  return {};
}

/// @brief Render a scope kind as its column text.
/// @param k The scope kind.
/// @return The column text.
auto scope_kind_to_text(scenario_scope_kind k) -> std::string_view {
  switch (k) {
  case scenario_scope_kind::global:
    return "global";
  case scenario_scope_kind::association:
    return "association";
  case scenario_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` onto this module's own
/// `scenario_scope_kind`. A 1:1 mapping kept explicit, matching
/// `decision.cpp`.
/// @param k The layer-1 scope kind.
/// @return This module's corresponding scope kind.
auto to_scenario_kind(scope_ref::scope_kind k) -> scenario_scope_kind {
  switch (k) {
  case scope_ref::scope_kind::global:
    return scenario_scope_kind::global;
  case scope_ref::scope_kind::association:
    return scenario_scope_kind::association;
  case scope_ref::scope_kind::repo:
    return scenario_scope_kind::repo;
  }
  return scenario_scope_kind::global; // unreachable
}

/// @brief Map a `planar.scope_ref` failure onto this module's error surface.
/// @param e The layer-1 failure.
/// @return The corresponding `scenario_error`.
auto to_scenario_error(scope_ref::error e) -> scenario_error {
  switch (e) {
  case scope_ref::error::query_failed:
    return scenario_error::query_failed;
  case scope_ref::error::slug_not_found:
    return scenario_error::slug_not_found;
  }
  return scenario_error::query_failed; // unreachable
}

/// @brief A resolved scope: the kind plus the row id (unset for global).
using resolved_scope = std::pair<scenario_scope_kind, std::optional<std::int64_t>>;

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
    -> std::expected<resolved_scope, scenario_error> {
  if (!scope.has_value()) {
    return std::make_pair(scenario_scope_kind::global, std::optional<std::int64_t>{});
  }
  auto resolved = scope_ref::resolve(conn, *scope);
  if (!resolved) {
    return std::unexpected(to_scenario_error(resolved.error()));
  }
  return std::make_pair(to_scenario_kind(resolved->kind), resolved->id);
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
auto resolve_scope_set(db::connection& conn, const scenario_list_filter& filter)
    -> std::expected<std::vector<resolved_scope>, scenario_error> {
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

constexpr std::string_view k_select_columns = "select id, scope_kind, scope_id, title, body, status, related_artifact_id, "
                                              "last_run_at, last_outcome, created_at, updated_at from test_scenarios";

/// @brief The status predicate for a list query.
///
/// An EMPTY status set yields NO PREDICATE AT ALL — every status is
/// returned, `retired` included. That is this family's own answer and it
/// differs from both siblings: `question`'s empty arm means `open` and
/// `decision`'s means `{proposed, accepted}`. See
/// `scenario_list_filter::statuses`.
/// @param filter The filter carrying the statuses.
/// @return The SQL fragment, leading space included, or `""`.
auto status_clause(const scenario_list_filter& filter) -> std::string {
  if (filter.statuses.empty()) {
    return {};
  }
  std::string out = " and status in (";
  for (std::size_t i = 0; i < filter.statuses.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += "?";
  }
  out += ")";
  return out;
}

/// @brief The scope disjunction for a list query.
///
/// `global` matches on `scope_kind` ALONE and the two other kinds are
/// emitted with their kind as a LITERAL, only the id bound — that is
/// scenario.zig's own shape, and it means the placeholder count differs
/// from `question`'s otherwise-identical clause. A copy from there would
/// bind one placeholder too many per non-global ref.
/// @param refs The resolved scope set; an empty set yields an empty string.
/// @return The SQL fragment, leading space included.
auto scope_clause(std::span<const resolved_scope> refs) -> std::string {
  if (refs.empty()) {
    return {};
  }
  std::string out = " and (";
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (i > 0) {
      out += " or ";
    }
    if (refs[i].first == scenario_scope_kind::global) {
      out += "scope_kind = 'global'";
    } else if (refs[i].first == scenario_scope_kind::association) {
      out += "(scope_kind = 'association' and scope_id = ?)";
    } else {
      out += "(scope_kind = 'repo' and scope_id = ?)";
    }
  }
  out += ")";
  return out;
}

/// @brief Decode one row of `k_select_columns` into a `scenario`.
/// @param stmt A statement positioned on a row.
/// @return The decoded row, or `query_failed` for an unparseable enum
/// column.
auto read_row(db::statement& stmt) -> std::expected<scenario, scenario_error> {
  const auto          scope_kind_text = stmt.column_text(1);
  scenario_scope_kind sk{};
  if (scope_kind_text == "global") {
    sk = scenario_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = scenario_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = scenario_scope_kind::repo;
  } else {
    return std::unexpected(scenario_error::query_failed);
  }

  const auto status = scenario_status_from_text(stmt.column_text(5));
  if (!status.has_value()) {
    return std::unexpected(scenario_error::query_failed);
  }

  // A NULL `last_outcome` stays unset. A NON-null but UNPARSEABLE one also
  // stays unset rather than failing the read: zig's `readRow` assigns
  // `Outcome.fromText(...)` — an OPTIONAL — straight into the field, so an
  // out-of-CHECK value degrades to "no outcome" there too. The CHECK
  // constraint makes it unreachable either way; reproduced so the two agree
  // if it ever is reached.
  std::optional<scenario_outcome> outcome;
  if (!stmt.is_null(8)) {
    outcome = scenario_outcome_from_text(stmt.column_text(8));
  }

  return scenario{
      .id         = stmt.column_int64(0),
      .scope_kind = sk,
      .scope_id   = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .title      = stmt.column_text(3),
      // `body` is nullable: an absent body is NULL and renders `null`, while
      // `--body ""` is `''` and renders `""`. The two are distinct values.
      .body                = stmt.is_null(4) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(4)},
      .status              = *status,
      .related_artifact_id = stmt.is_null(6) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(6)},
      .last_run_at         = stmt.is_null(7) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(7)},
      .last_outcome        = outcome,
      .created_at          = stmt.column_text(9),
      .updated_at          = stmt.column_text(10),
  };
}

/// @brief Read every remaining row of `stmt` into a vector.
/// @param stmt A bound, un-stepped statement over `k_select_columns`.
/// @return The rows in statement order, or the first decode/step failure.
auto collect_rows(db::statement& stmt) -> std::expected<std::vector<scenario>, scenario_error> {
  std::vector<scenario> out;
  for (;;) {
    auto step = stmt.step();
    if (!step) {
      return std::unexpected(scenario_error::query_failed);
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

/// @brief Validate a scenario status move.
///
/// Unlike `decision.cpp`'s equivalent, this does NOT rewrite the matrix's
/// vocabulary: zig's scenario paths `try policy.status.check(...)` and let
/// `IllegalTransition` / `UnknownStatus` propagate out of the engine
/// unfolded, so the operator sees those tags verbatim. Folding them into a
/// "terminal" spelling would change the error text on every refusal.
/// @param current The row's current status.
/// @param next The status being moved to.
/// @return Success, or the mapped refusal.
auto validate_transition(scenario_status current, scenario_status next) -> std::expected<void, scenario_error> {
  auto checked =
      check_transition(transition_kind::scenario, scenario_status_to_text(current), scenario_status_to_text(next), false);
  if (checked) {
    return {};
  }
  return std::unexpected(checked.error() == transition_error::unknown_status ? scenario_error::unknown_status
                                                                             : scenario_error::illegal_transition);
}

/// @brief Run one parameterless UPDATE against `test_scenarios`.
/// @param conn An open, migrated connection.
/// @param sql The statement, whose sole placeholder is the row id.
/// @param id The scenario's row id.
/// @return Whether it ran.
auto exec_update(db::connection& conn, std::string_view sql, std::int64_t id) -> bool {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return false;
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return false;
  }
  return stmt->step().has_value();
}

/// @brief Shared body of `ready_scenario` / `retire_scenario`.
///
/// Both read the current row, validate the transition, set their status,
/// write one `status_change` audit row and re-read — inside one transaction
/// so a failed audit write takes the status flip with it.
/// @param conn An open, migrated connection.
/// @param id The scenario's row id.
/// @param target The status being moved to.
/// @param summary The `audit_log` summary to record.
/// @return The updated row, or the first failure.
auto simple_transition(db::connection& conn, std::int64_t id, scenario_status target, std::string summary)
    -> std::expected<scenario, scenario_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(scenario_error::query_failed);
  }

  auto current = show_scenario(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (auto checked = validate_transition(current->status, target); !checked) {
    return std::unexpected(checked.error());
  }

  if (!exec_update(conn,
                   std::format("update test_scenarios set status = '{}', "
                               "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
                               scenario_status_to_text(target)),
                   id)) {
    return std::unexpected(scenario_error::query_failed);
  }

  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "scenario", .id = id},
                                                     .summary = std::move(summary)});
      !a) {
    return std::unexpected(a.error());
  }

  auto updated = show_scenario(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(scenario_error::query_failed);
  }
  return updated;
}

} // namespace

auto scenario_status_from_text(std::string_view s) -> std::optional<scenario_status> {
  if (s == "draft") {
    return scenario_status::draft;
  }
  if (s == "ready") {
    return scenario_status::ready;
  }
  if (s == "verified") {
    return scenario_status::verified;
  }
  if (s == "failing") {
    return scenario_status::failing;
  }
  if (s == "retired") {
    return scenario_status::retired;
  }
  return std::nullopt;
}

auto scenario_status_to_text(scenario_status s) -> std::string_view {
  switch (s) {
  case scenario_status::draft:
    return "draft";
  case scenario_status::ready:
    return "ready";
  case scenario_status::verified:
    return "verified";
  case scenario_status::failing:
    return "failing";
  case scenario_status::retired:
    return "retired";
  }
  return "draft"; // unreachable
}

auto scenario_outcome_from_text(std::string_view s) -> std::optional<scenario_outcome> {
  if (s == "pass") {
    return scenario_outcome::pass;
  }
  if (s == "fail") {
    return scenario_outcome::fail;
  }
  if (s == "error") {
    return scenario_outcome::error_case;
  }
  if (s == "skipped") {
    return scenario_outcome::skipped;
  }
  return std::nullopt;
}

auto scenario_outcome_to_text(scenario_outcome o) -> std::string_view {
  switch (o) {
  case scenario_outcome::pass:
    return "pass";
  case scenario_outcome::fail:
    return "fail";
  case scenario_outcome::error_case:
    return "error";
  case scenario_outcome::skipped:
    return "skipped";
  }
  return "pass"; // unreachable
}

auto create_scenario(db::connection& conn, const scenario_create_args& args) -> std::expected<scenario, scenario_error> {
  auto scope = resolve_scope_or_global(conn, args.scope);
  if (!scope) {
    return std::unexpected(scope.error());
  }

  // `--plan` is validated BEFORE the INSERT, so a dangling plan id leaves
  // neither a `test_scenarios` row, nor an `audit_log` row, nor an
  // `entity_links` edge (task 6197).
  //
  // This verb used to answer its two reference flags DIFFERENTLY: `--related
  // 77` refused, because `related_artifact_id references artifacts(id)` is a
  // real foreign key and SQLite did the refusing, while `--plan 4242` exited
  // 0 and left a dangling edge, because `entity_links` carries no foreign key
  // to its target table. The difference was structural rather than a policy
  // choice, which is why it is closed in code rather than by a migration.
  if (args.plan_id.has_value()) {
    auto stmt = conn.prepare("select count(*) from plans where id = ?");
    if (!stmt) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, *args.plan_id); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    auto step = stmt->step();
    if (!step || *step != db::step_result::row) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (stmt->column_int64(0) == 0) {
      return std::unexpected(scenario_error::not_found);
    }
  }

  std::int64_t id = 0;
  {
    // `status` is NOT in the column list: the row takes the column's own
    // `default 'draft'`, exactly as the oracle's INSERT does.
    auto stmt = conn.prepare("insert into test_scenarios (scope_kind, scope_id, title, body, related_artifact_id) "
                             "values (?, ?, ?, ?, ?) returning id");
    if (!stmt) {
      return std::unexpected(exec_failed("scenario.create", "PrepareFailed"));
    }
    if (auto b = stmt->bind_text(1, scope_kind_to_text(scope->first)); !b) {
      return std::unexpected(exec_failed("scenario.create", "BindFailed"));
    }
    auto b2 = scope->second.has_value() ? stmt->bind_int64(2, *scope->second) : stmt->bind_null(2);
    if (!b2) {
      return std::unexpected(exec_failed("scenario.create", "BindFailed"));
    }
    if (auto b = stmt->bind_text(3, args.title); !b) {
      return std::unexpected(exec_failed("scenario.create", "BindFailed"));
    }
    // `body` binds SQL NULL when absent, NOT the empty string: `render_json`
    // emits `null` vs `""` and the two are operator-visible.
    auto b4 = args.body.has_value() ? stmt->bind_text(4, *args.body) : stmt->bind_null(4);
    if (!b4) {
      return std::unexpected(exec_failed("scenario.create", "BindFailed"));
    }
    auto b5 = args.related_artifact_id.has_value() ? stmt->bind_int64(5, *args.related_artifact_id) : stmt->bind_null(5);
    if (!b5) {
      return std::unexpected(exec_failed("scenario.create", "BindFailed"));
    }
    auto step = stmt->step();
    if (!step) {
      // This is where a nonexistent `--related` artifact lands: the
      // `references artifacts(id)` foreign key fails the INSERT and NO row
      // is written. Oracle-captured as exit 1 / `scenario add: QueryFailed`.
      // The oracle's execParams catch has no unique-violation special
      // case here (unlike plan.create/task.create) -- StepFailed applies
      // uniformly, including to this FK-violation path.
      return std::unexpected(exec_failed("scenario.create", "StepFailed"));
    }
    if (*step != db::step_result::row) {
      return std::unexpected(scenario_error::query_failed);
    }
    id = stmt->column_int64(0);
  }

  // ORACLE: `create|scenario|1|create scenario 'T1'` — the TITLE in single
  // quotes, and the entity kind is `scenario`, not `test_scenario`. The
  // entity_links `from_kind` below IS `test_scenario`; the two spellings
  // genuinely differ and both were captured.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "scenario", .id = id},
                                                     .summary = std::format("create scenario '{}'", args.title)});
      !a) {
    return std::unexpected(a.error());
  }

  if (args.plan_id.has_value()) {
    // No `link` audit row here — the oracle writes exactly one row for
    // `scenario add --plan`, and its verb is `create`. The plan is NOT
    // checked for existence; see scenario.cppm's header.
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                             "values ('test_scenario', ?, 'plan', ?, 'derives-from')");
    if (!stmt) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, *args.plan_id); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (!stmt->step().has_value()) {
      return std::unexpected(scenario_error::query_failed);
    }
  }

  return show_scenario(conn, id);
}

auto show_scenario(db::connection& conn, std::int64_t id) -> std::expected<scenario, scenario_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(scenario_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(scenario_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(scenario_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(scenario_error::not_found);
  }
  return read_row(*stmt);
}

auto list_scenarios(db::connection& conn, const scenario_list_filter& filter)
    -> std::expected<std::vector<scenario>, scenario_error> {
  auto refs = resolve_scope_set(conn, filter);
  if (!refs) {
    return std::unexpected(refs.error());
  }

  std::string sql = std::string(k_select_columns) + " where 1 = 1";
  sql += status_clause(filter);
  if (filter.related_artifact_id.has_value()) {
    sql += " and related_artifact_id = ?";
  }
  sql += scope_clause(*refs);
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(scenario_error::query_failed);
  }
  int idx = 1;
  for (const auto s : filter.statuses) {
    if (auto b = stmt->bind_text(idx++, scenario_status_to_text(s)); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
  }
  if (filter.related_artifact_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.related_artifact_id); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
  }
  for (const auto& ref : *refs) {
    if (ref.first == scenario_scope_kind::global) {
      continue; // literal in the SQL; no placeholder to fill.
    }
    if (!ref.second.has_value()) {
      // A non-global ref with no row id cannot become a predicate. The
      // oracle unwraps the optional unconditionally and would panic; refuse
      // rather than bind a placeholder id, which would silently match
      // nothing and return a short list.
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *ref.second); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
  }

  return collect_rows(*stmt);
}

auto list_scenarios_touching(db::connection& conn, std::int64_t repo_id, const scenario_list_filter& filter)
    -> std::expected<std::vector<scenario>, scenario_error> {
  auto refs = resolve_scope_set(conn, filter);
  if (!refs) {
    return std::unexpected(refs.error());
  }

  // The direct-repo-scope arm is ALL-OR-NOTHING against the scope set — see
  // scenario.cppm. With no scope set at all it stays on.
  bool branch_direct = true;
  if (!refs->empty()) {
    branch_direct = false;
    for (const auto& ref : *refs) {
      if (ref.first == scenario_scope_kind::repo && (!ref.second.has_value() || *ref.second == repo_id)) {
        branch_direct = true;
        break;
      }
    }
  }

  const auto status_sql = status_clause(filter);

  std::string sql = "select * from (";
  sql += k_select_columns;
  sql += " where 1 = 1";
  if (branch_direct) {
    sql += " and scope_kind = 'repo' and scope_id = ?";
    sql += status_sql;
    if (filter.related_artifact_id.has_value()) {
      sql += " and related_artifact_id = ?";
    }
  } else {
    // An arm that is off is emitted as `1 = 0` rather than dropped, so the
    // UNION keeps both arms and the column lists stay identical.
    sql += " and 1 = 0";
  }
  sql += " union ";
  sql += k_select_columns;
  sql += " where 1 = 1";
  sql += " and id in (select from_id from entity_links where from_kind = 'test_scenario' and to_kind = 'repo' and to_id = ? "
         "and relationship = 'touches')";
  sql += status_sql;
  if (filter.related_artifact_id.has_value()) {
    sql += " and related_artifact_id = ?";
  }
  // The scope predicate applies to the TOUCHES arm only. The direct arm has
  // already been gated by `branch_direct` — applying it there too would
  // double-filter and drop the repo-scoped rows the arm exists to return.
  sql += scope_clause(*refs);
  sql += ") order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(scenario_error::query_failed);
  }
  int  idx       = 1;
  auto bind_int  = [&](std::int64_t v) { return stmt->bind_int64(idx++, v).has_value(); };
  auto bind_text = [&](std::string_view v) { return stmt->bind_text(idx++, v).has_value(); };

  if (branch_direct) {
    if (!bind_int(repo_id)) {
      return std::unexpected(scenario_error::query_failed);
    }
    for (const auto s : filter.statuses) {
      if (!bind_text(scenario_status_to_text(s))) {
        return std::unexpected(scenario_error::query_failed);
      }
    }
    if (filter.related_artifact_id.has_value() && !bind_int(*filter.related_artifact_id)) {
      return std::unexpected(scenario_error::query_failed);
    }
  }
  if (!bind_int(repo_id)) {
    return std::unexpected(scenario_error::query_failed);
  }
  for (const auto s : filter.statuses) {
    if (!bind_text(scenario_status_to_text(s))) {
      return std::unexpected(scenario_error::query_failed);
    }
  }
  if (filter.related_artifact_id.has_value() && !bind_int(*filter.related_artifact_id)) {
    return std::unexpected(scenario_error::query_failed);
  }
  for (const auto& ref : *refs) {
    if (ref.first == scenario_scope_kind::global) {
      continue;
    }
    if (!ref.second.has_value()) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (!bind_int(*ref.second)) {
      return std::unexpected(scenario_error::query_failed);
    }
  }

  return collect_rows(*stmt);
}

auto verify_scenario(db::connection& conn, std::int64_t id, scenario_outcome outcome, std::optional<std::string_view> summary)
    -> std::expected<scenario, scenario_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(scenario_error::query_failed);
  }

  auto current = show_scenario(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  const auto outcome_text = scenario_outcome_to_text(outcome);

  if (outcome == scenario_outcome::pass) {
    // Auto-transition: a `draft` scenario is walked to `ready` FIRST, with
    // its own matrix check and its own audit row, and only then to
    // `verified`. `draft -> verified` is not an edge and this is why the
    // operator never has to run a `ready` verb that does not exist.
    auto source = current->status;
    if (source == scenario_status::draft) {
      if (auto checked = validate_transition(scenario_status::draft, scenario_status::ready); !checked) {
        return std::unexpected(checked.error());
      }
      if (!exec_update(conn,
                       "update test_scenarios set status = 'ready', "
                       "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
                       id)) {
        return std::unexpected(scenario_error::query_failed);
      }
      // ORACLE, verbatim. This row is the ONLY observable difference
      // between a draft-to-verified call and a ready-to-verified one.
      if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                         .entity  = {.kind = "scenario", .id = id},
                                                         .summary = "ready: auto-transition via verify"});
          !a) {
        return std::unexpected(a.error());
      }
      source = scenario_status::ready;
    }
    // The second hop uses the status the row is ACTUALLY at now, not the
    // one it was read at.
    if (auto checked = validate_transition(source, scenario_status::verified); !checked) {
      // The transaction is NOT committed, so the destructor rolls the
      // auto-`ready` hop back on the way out. Unreachable from `ready`
      // itself, but a `retired` source lands here having written nothing —
      // which is what makes `scenario verify <retired>` leave `updated_at`
      // unmoved.
      return std::unexpected(checked.error());
    }

    auto stmt = conn.prepare("update test_scenarios set status = 'verified', last_outcome = ?, "
                             "last_run_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    if (!stmt) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_text(1, outcome_text); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, id); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (!stmt->step().has_value()) {
      return std::unexpected(scenario_error::query_failed);
    }
  } else {
    // A NON-passing run records the outcome and the run stamp but leaves
    // `status` alone, and NEVER consults the matrix — which is why a
    // `retired` scenario accepts `--outcome skipped` while refusing the
    // default `pass`.
    auto stmt = conn.prepare("update test_scenarios set last_outcome = ?, "
                             "last_run_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    if (!stmt) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_text(1, outcome_text); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, id); !b) {
      return std::unexpected(scenario_error::query_failed);
    }
    if (!stmt->step().has_value()) {
      return std::unexpected(scenario_error::query_failed);
    }
  }

  // ORACLE: `verify: pass` with no `--summary`, `verify(fail): broke` with
  // one. Note the outcome moves INSIDE the parentheses when a summary is
  // present — it is not appended.
  auto audit_summary =
      summary.has_value() ? std::format("verify({}): {}", outcome_text, *summary) : std::format("verify: {}", outcome_text);
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "scenario", .id = id},
                                                     .summary = std::move(audit_summary)});
      !a) {
    return std::unexpected(a.error());
  }

  auto updated = show_scenario(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(scenario_error::query_failed);
  }
  return updated;
}

auto ready_scenario(db::connection& conn, std::int64_t id, std::optional<std::string_view> reason)
    -> std::expected<scenario, scenario_error> {
  return simple_transition(conn, id, scenario_status::ready,
                           reason.has_value() ? std::format("ready: {}", *reason) : std::string{"ready"});
}

auto retire_scenario(db::connection& conn, std::int64_t id, std::optional<std::string_view> reason)
    -> std::expected<scenario, scenario_error> {
  return simple_transition(conn, id, scenario_status::retired,
                           reason.has_value() ? std::format("retire: {}", *reason) : std::string{"retire"});
}

auto render_text(const scenario& s) -> std::string {
  std::string out;
  out += std::format("id:         {}\n", s.id);
  out += std::format("title:      {}\n", s.title);
  out += std::format("status:     {}\n", scenario_status_to_text(s.status));
  out += std::format("scope:      {}", scope_kind_to_text(s.scope_kind));
  if (s.scope_id.has_value()) {
    out += std::format(":{}", *s.scope_id);
  }
  out += "\n";
  if (s.related_artifact_id.has_value()) {
    out += std::format("artifact:   {}\n", *s.related_artifact_id);
  }
  if (s.last_outcome.has_value()) {
    out += std::format("outcome:    {}\n", scenario_outcome_to_text(*s.last_outcome));
  }
  if (s.last_run_at.has_value()) {
    out += std::format("last run:   {}\n", *s.last_run_at);
  }
  // `body:` is CONDITIONAL and comes LAST of the four, after the run
  // columns — where `decision`'s `body:` is unconditional and comes first.
  if (s.body.has_value()) {
    out += std::format("body:       {}\n", *s.body);
  }
  out += std::format("created:    {}\n", s.created_at);
  out += std::format("updated:    {}\n", s.updated_at);
  return out;
}

auto render_json(const scenario& s) -> std::string {
  auto const opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  auto const opt_int = [](const std::optional<std::int64_t>& v) -> std::string {
    return v.has_value() ? std::format("{}", *v) : std::string{"null"};
  };
  auto const outcome =
      s.last_outcome.has_value() ? std::format("\"{}\"", scenario_outcome_to_text(*s.last_outcome)) : std::string{"null"};
  return std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},"title":{},"body":{},"status":"{}",)"
                     R"("related_artifact_id":{},"last_run_at":{},"last_outcome":{},"created_at":{},"updated_at":{}}})",
                     s.id, scope_kind_to_text(s.scope_kind), opt_int(s.scope_id), json_string(s.title), opt_str(s.body),
                     scenario_status_to_text(s.status), opt_int(s.related_artifact_id), opt_str(s.last_run_at), outcome,
                     json_string(s.created_at), json_string(s.updated_at));
}

auto render_list_text(std::span<const scenario> items) -> std::string {
  if (items.empty()) {
    // WITH parentheses, matching `question`'s `(no questions)`. `decision`
    // renders a bare `no decisions`; the difference was captured, not
    // inherited.
    return "(no scenarios)\n";
  }
  std::string out;
  for (const auto& s : items) {
    // A null outcome is a single `-`, still padded to the eight-wide
    // column. Dropping the padding would left-shift every title on any list
    // holding a mix of run and un-run scenarios.
    const std::string outcome{s.last_outcome.has_value() ? scenario_outcome_to_text(*s.last_outcome) : std::string_view{"-"}};
    out += std::format("{:>5}  {:<10}  {:<8}  {}\n", s.id, scenario_status_to_text(s.status), outcome, s.title);
  }
  return out;
}

auto render_list_json(std::span<const scenario> items) -> std::string {
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
