/// @file agentactivity.cpp
/// @brief Implementation of `planar.engine.runtime.agentactivity`. See the
/// module interface for the contract, the time model, and the deliberate
/// omissions.

module;

// `pid_alive` is the one place in this module that leaves SQLite: the run
// sweep decides a `workflow_runs` row is dead by asking the kernel whether
// its recorded pid still exists. `kill(pid, 0)` sends no signal — it runs
// only the permission and existence checks — which is why this is a probe
// and not an act.
#include <cerrno>
#include <csignal>

module planar.engine.runtime.agentactivity;

import std;
import planar.db;

namespace planar::engine::runtime::agentactivity {

namespace {

/// @brief The SQL expression every timestamp in this subsystem comes from.
/// Spelled once so a typo cannot make two columns disagree about "now".
constexpr std::string_view k_now = "strftime('%Y-%m-%dT%H:%M:%fZ','now')";

/// @brief Bind an optional text parameter: the value, or SQL NULL.
/// @param stmt The statement.
/// @param index The 1-indexed parameter position.
/// @param value The value, or unset for NULL.
/// @return `true` on success.
auto bind_opt_text(db::statement& stmt, int index, std::optional<std::string_view> value) -> bool {
  if (value.has_value()) {
    return stmt.bind_text(index, *value).has_value();
  }
  return stmt.bind_null(index).has_value();
}

/// @brief Bind an optional integer parameter: the value, or SQL NULL.
/// @param stmt The statement.
/// @param index The 1-indexed parameter position.
/// @param value The value, or unset for NULL.
/// @return `true` on success.
auto bind_opt_int(db::statement& stmt, int index, std::optional<std::int64_t> value) -> bool {
  if (value.has_value()) {
    return stmt.bind_int64(index, *value).has_value();
  }
  return stmt.bind_null(index).has_value();
}

/// @brief Read a nullable text column.
/// @param stmt The statement positioned on a row.
/// @param index The 0-indexed column.
/// @return The text, or unset when NULL.
auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

/// @brief Read a nullable integer column.
/// @param stmt The statement positioned on a row.
/// @param index The 0-indexed column.
/// @return The value, or unset when NULL.
auto opt_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_int64(index);
}

/// @brief Run a statement that returns a single integer.
/// @param conn The connection.
/// @param sql The SQL to run.
/// @return The value, or unset when the statement failed or produced no row.
auto int_query(db::connection& conn, std::string_view sql) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief The number of rows the most recent statement on `conn` changed.
///
/// `select changes()` rather than a `sqlite3_changes` accessor, because
/// `planar.db` exposes no such accessor and the Zig original reaches the
/// same number the same way in `associateClaimRun`. `changes()` is not
/// disturbed by the SELECT that reads it (SQLite excludes statements that
/// modify no rows), so this is safe to call immediately after an UPDATE.
/// @param conn The connection.
/// @return The row count; `0` when the probe itself failed.
auto changes(db::connection& conn) -> std::int64_t {
  return int_query(conn, "select changes()").value_or(0);
}

/// @brief The rowid of the most recent successful INSERT on `conn`.
/// @param conn The connection.
/// @return The rowid, or unset when the probe failed.
auto last_insert_rowid(db::connection& conn) -> std::optional<std::int64_t> {
  return int_query(conn, "select last_insert_rowid()");
}

/// @brief Render a signed seconds offset as a SQLite `strftime` modifier.
///
/// `'+600 seconds'` for non-negative, `'-600 seconds'` for negative — NOT
/// `'+-600 seconds'`, which SQLite parses as a syntax error and silently
/// evaluates to NULL. See the module header.
/// @param secs The offset in seconds.
/// @return The modifier text, quotes included.
auto seconds_modifier(std::int64_t secs) -> std::string {
  return secs >= 0 ? std::format("'+{} seconds'", secs) : std::format("'{} seconds'", secs);
}

/// @brief The canonical 27-column claim projection, shared by every claim
/// SELECT so the column ORDER lives in exactly one place. `read_claim_row`
/// depends on this order.
constexpr std::string_view k_claim_columns = "select id, claim_token, session_id, entity_kind, entity_id, claim_scope,\n"
                                             "       status, vendor, vendor_session_id, role, model,\n"
                                             "       worktree_id, worktree_path,\n"
                                             "       repo_root, branch, head_sha_at_claim, dirty_at_claim,\n"
                                             "       purpose, base_ref,\n"
                                             "       claimed_at, last_heartbeat_at, lease_expires_at,\n"
                                             "       released_at, release_reason,\n"
                                             "       run_id, stage, failure_category\n"
                                             "from agent_work_claims\n";

/// @brief Decode one row of `k_claim_columns`.
///
/// An unparseable `entity_kind`, `claim_scope`, `status` or
/// `failure_category` is `query_failed` — those columns carry CHECK
/// constraints, so a value outside the set means the row is corrupt and
/// guessing at it would make a corrupt row look healthy. `dirty_at_claim`
/// is the deliberate exception: it degrades to unset, matching the Zig
/// original.
/// @param stmt The statement positioned on a row.
/// @return The decoded claim, or `query_failed`.
auto read_claim_row(const db::statement& stmt) -> std::expected<claim, agent_error> {
  claim row;
  row.id          = stmt.column_int64(0);
  row.claim_token = stmt.column_text(1);
  row.session_id  = stmt.column_int64(2);
  auto const kind = entity_kind_from_text(stmt.column_text(3));
  if (!kind) {
    return std::unexpected(agent_error::query_failed);
  }
  row.kind         = *kind;
  row.entity_id    = stmt.column_int64(4);
  auto const scope = claim_scope_from_text(stmt.column_text(5));
  if (!scope) {
    return std::unexpected(agent_error::query_failed);
  }
  row.scope         = *scope;
  auto const status = claim_status_from_text(stmt.column_text(6));
  if (!status) {
    return std::unexpected(agent_error::query_failed);
  }
  row.status            = *status;
  row.vendor            = stmt.column_text(7);
  row.vendor_session_id = opt_text(stmt, 8);
  row.role              = opt_text(stmt, 9);
  row.model             = opt_text(stmt, 10);
  row.worktree_id       = opt_int(stmt, 11);
  row.worktree_path     = opt_text(stmt, 12);
  row.repo_root         = opt_text(stmt, 13);
  row.branch            = opt_text(stmt, 14);
  row.head_sha_at_claim = opt_text(stmt, 15);
  if (auto const dirty = opt_text(stmt, 16); dirty.has_value()) {
    row.dirty_at_claim = dirty_state_from_text(*dirty);
  }
  row.purpose           = opt_text(stmt, 17);
  row.base_ref          = opt_text(stmt, 18);
  row.claimed_at        = stmt.column_text(19);
  row.last_heartbeat_at = stmt.column_text(20);
  row.lease_expires_at  = stmt.column_text(21);
  row.released_at       = opt_text(stmt, 22);
  row.release_reason    = opt_text(stmt, 23);
  row.run_id            = opt_int(stmt, 24);
  row.stage             = opt_text(stmt, 25);
  if (auto const category = opt_text(stmt, 26); category.has_value()) {
    auto const parsed = failure_category_from_text(*category);
    if (!parsed) {
      return std::unexpected(agent_error::query_failed);
    }
    row.category = parsed;
  }
  return row;
}

/// @brief The canonical 18-column action projection.
constexpr std::string_view k_action_columns = "select id, session_id, session_entry_id, parent_action_id, claim_id,\n"
                                              "       action_kind, entity_kind, entity_id,\n"
                                              "       vendor, vendor_role, model,\n"
                                              "       started_at, ended_at, outcome, summary,\n"
                                              "       head_sha, dirty,\n"
                                              "       metadata\n"
                                              "from agent_actions\n";

/// @brief Decode one row of `k_action_columns`.
/// @param stmt The statement positioned on a row.
/// @return The decoded action, or `query_failed`.
auto read_action_row(const db::statement& stmt) -> std::expected<action, agent_error> {
  action row;
  row.id               = stmt.column_int64(0);
  row.session_id       = stmt.column_int64(1);
  row.session_entry_id = opt_int(stmt, 2);
  row.parent_action_id = opt_int(stmt, 3);
  row.claim_id         = opt_int(stmt, 4);
  auto const kind      = action_kind_from_text(stmt.column_text(5));
  if (!kind) {
    return std::unexpected(agent_error::query_failed);
  }
  row.kind = *kind;
  if (auto const entity = opt_text(stmt, 6); entity.has_value()) {
    auto const parsed = action_entity_kind_from_text(*entity);
    if (!parsed) {
      return std::unexpected(agent_error::query_failed);
    }
    row.entity = parsed;
  }
  row.entity_id   = opt_int(stmt, 7);
  row.vendor      = stmt.column_text(8);
  row.vendor_role = opt_text(stmt, 9);
  row.model       = opt_text(stmt, 10);
  row.started_at  = stmt.column_text(11);
  row.ended_at    = opt_text(stmt, 12);
  if (auto const result = opt_text(stmt, 13); result.has_value()) {
    auto const parsed = outcome_from_text(*result);
    if (!parsed) {
      return std::unexpected(agent_error::query_failed);
    }
    row.result = parsed;
  }
  row.summary  = opt_text(stmt, 14);
  row.head_sha = opt_text(stmt, 15);
  if (auto const dirty = opt_text(stmt, 16); dirty.has_value()) {
    row.dirty = dirty_state_from_text(*dirty);
  }
  row.metadata = opt_text(stmt, 17);
  return row;
}

/// @brief Fetch a single claim through the shared projection plus `tail`.
/// @param conn The connection.
/// @param tail The WHERE clause completing `k_claim_columns`.
/// @param bind Applies the tail's bound parameters.
/// @return The row, or `claim_not_found` / `query_failed`.
auto fetch_claim(db::connection& conn, std::string_view tail, const std::function<bool(db::statement&)>& bind)
    -> std::expected<claim, agent_error> {
  auto stmt = conn.prepare(std::string{k_claim_columns} + std::string{tail});
  if (!stmt || !bind(*stmt)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(agent_error::claim_not_found);
  }
  return read_claim_row(*stmt);
}

/// @brief Is there a live claim on `(kind, id)` right now?
///
/// The exclusivity predicate. Note what it does NOT consult:
/// `claim_scope`. See the enum's own doc comment.
/// @param conn The connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return `true` when a live claim exists, or `query_failed`.
auto has_active_claim(db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<bool, agent_error> {
  auto stmt = conn.prepare(std::format("select 1 from agent_work_claims\n"
                                       "where entity_kind = ?\n"
                                       "  and entity_id = ?\n"
                                       "  and status = 'active'\n"
                                       "  and lease_expires_at >= {}\n"
                                       "limit 1",
                                       k_now));
  if (!stmt || !stmt->bind_text(1, to_text(kind)) || !stmt->bind_int64(2, id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

/// @brief Mark every active claim on `(kind, id)` stale, for `--force`.
///
/// Deliberately carries NO lease guard and does NOT set
/// `failure_category` — a takeover sweeps live and expired claims alike,
/// and the category column belongs to the failure taxonomy, not to
/// operator recovery. Zig's behavior.
/// @param conn The connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return Success, or `query_failed`.
auto mark_active_stale(db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<void, agent_error> {
  auto stmt = conn.prepare(std::format("update agent_work_claims\n"
                                       "set status = 'stale',\n"
                                       "    released_at = {},\n"
                                       "    release_reason = ?\n"
                                       "where entity_kind = ?\n"
                                       "  and entity_id = ?\n"
                                       "  and status = 'active'",
                                       k_now));
  if (!stmt || !stmt->bind_text(1, "force takeover") || !stmt->bind_text(2, to_text(kind)) || !stmt->bind_int64(3, id)) {
    return std::unexpected(agent_error::query_failed);
  }
  if (!stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  return {};
}

/// @brief Refuse a `--worktree <id>` that names no row.
///
/// Tolerant of the table not existing at all: `worktree_id` is
/// deliberately not a foreign key, so a database whose schema predates the
/// `worktrees` table passes the id through opaquely rather than failing.
/// @param conn The connection.
/// @param worktree_id The id to validate.
/// @return Success, or `worktree_not_found` / `query_failed`.
auto validate_worktree_id(db::connection& conn, std::int64_t worktree_id) -> std::expected<void, agent_error> {
  auto const present = int_query(conn, "select count(*) from sqlite_master where type='table' and name='worktrees'");
  if (!present.has_value()) {
    return std::unexpected(agent_error::query_failed);
  }
  if (*present == 0) {
    return {};
  }
  auto stmt = conn.prepare("select 1 from worktrees where id = ?");
  if (!stmt || !stmt->bind_int64(1, worktree_id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(agent_error::worktree_not_found);
  }
  return {};
}

/// @brief The `dirty_at_claim` text a CLAIM insert should store.
///
/// NULL only when the whole snapshot is empty. Note this predicate is NOT
/// the one `start_action` uses for the same conceptual column — the Zig
/// original genuinely differs between the two call sites, and both are
/// reproduced rather than unified.
/// @param loc The snapshot.
/// @return The token, or unset for NULL.
auto claim_dirty_text(const locality& loc) -> std::optional<std::string_view> {
  if (loc.dirty == dirty_state::unknown && !loc.repo_root.has_value() && !loc.branch.has_value() && !loc.head_sha.has_value()) {
    return std::nullopt;
  }
  return to_text(loc.dirty);
}

/// @brief The `dirty` text an ACTION insert should store.
///
/// NULL when there is no `head_sha` AND the state is unknown — a laxer
/// predicate than `claim_dirty_text`'s, deliberately. See that function.
/// @param loc The snapshot.
/// @return The token, or unset for NULL.
auto action_dirty_text(const locality& loc) -> std::optional<std::string_view> {
  if (!loc.head_sha.has_value() && loc.dirty == dirty_state::unknown) {
    return std::nullopt;
  }
  return to_text(loc.dirty);
}

/// @brief View an optional string as an optional string_view.
/// @param value The owning optional.
/// @return A view over it, or unset.
auto view(const std::optional<std::string>& value) -> std::optional<std::string_view> {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*value};
}

} // namespace

// =========================================================================
// Enumeration text
// =========================================================================

auto to_text(entity_kind value) -> std::string_view {
  switch (value) {
  case entity_kind::plan:
    return "plan";
  case entity_kind::plan_step:
    return "plan_step";
  case entity_kind::task:
    return "task";
  }
  return "task";
}

auto to_text(claim_scope value) -> std::string_view {
  return value == claim_scope::shared ? "shared" : "exclusive";
}

auto to_text(claim_status value) -> std::string_view {
  switch (value) {
  case claim_status::active:
    return "active";
  case claim_status::released:
    return "released";
  case claim_status::completed:
    return "completed";
  case claim_status::aborted:
    return "aborted";
  case claim_status::stale:
    return "stale";
  }
  return "active";
}

auto to_text(failure_category value) -> std::string_view {
  switch (value) {
  case failure_category::usage_limit:
    return "usage_limit";
  case failure_category::context_limit:
    return "context_limit";
  case failure_category::output_limit:
    return "output_limit";
  case failure_category::tool_failure:
    return "tool_failure";
  case failure_category::validation:
    return "validation";
  case failure_category::unknown:
    return "unknown";
  }
  return "unknown";
}

auto to_text(action_kind value) -> std::string_view {
  switch (value) {
  case action_kind::planner:
    return "planner";
  case action_kind::ingestor:
    return "ingestor";
  case action_kind::coder:
    return "coder";
  case action_kind::test_coder:
    return "test_coder";
  case action_kind::reviewer:
    return "reviewer";
  case action_kind::ext_sync:
    return "ext_sync";
  case action_kind::ext_propagate:
    return "ext_propagate";
  case action_kind::orchestrator:
    return "orchestrator";
  case action_kind::resume_:
    return "resume";
  case action_kind::workbench_sync:
    return "workbench_sync";
  case action_kind::spec_draft:
    return "spec_draft";
  case action_kind::claim_check:
    return "claim_check";
  case action_kind::heartbeat:
    return "heartbeat";
  case action_kind::tool_call:
    return "tool_call";
  case action_kind::user_message:
    return "user_message";
  case action_kind::assistant_message:
    return "assistant_message";
  case action_kind::other:
    return "other";
  }
  return "other";
}

auto to_text(action_entity_kind value) -> std::string_view {
  switch (value) {
  case action_entity_kind::plan:
    return "plan";
  case action_entity_kind::plan_step:
    return "plan_step";
  case action_entity_kind::task:
    return "task";
  case action_entity_kind::question:
    return "question";
  case action_entity_kind::test_scenario:
    return "test_scenario";
  case action_entity_kind::artifact:
    return "artifact";
  case action_entity_kind::decision:
    return "decision";
  }
  return "task";
}

auto to_text(outcome value) -> std::string_view {
  switch (value) {
  case outcome::ok:
    return "ok";
  case outcome::error_:
    return "error";
  case outcome::aborted:
    return "aborted";
  case outcome::timeout:
    return "timeout";
  }
  return "ok";
}

auto to_text(dirty_state value) -> std::string_view {
  switch (value) {
  case dirty_state::clean:
    return "clean";
  case dirty_state::dirty:
    return "dirty";
  case dirty_state::unknown:
    return "unknown";
  }
  return "unknown";
}

auto entity_kind_from_text(std::string_view text) -> std::optional<entity_kind> {
  if (text == "plan") {
    return entity_kind::plan;
  }
  if (text == "plan_step") {
    return entity_kind::plan_step;
  }
  if (text == "task") {
    return entity_kind::task;
  }
  return std::nullopt;
}

auto claim_scope_from_text(std::string_view text) -> std::optional<claim_scope> {
  if (text == "exclusive") {
    return claim_scope::exclusive;
  }
  if (text == "shared") {
    return claim_scope::shared;
  }
  return std::nullopt;
}

auto claim_status_from_text(std::string_view text) -> std::optional<claim_status> {
  if (text == "active") {
    return claim_status::active;
  }
  if (text == "released") {
    return claim_status::released;
  }
  if (text == "completed") {
    return claim_status::completed;
  }
  if (text == "aborted") {
    return claim_status::aborted;
  }
  if (text == "stale") {
    return claim_status::stale;
  }
  return std::nullopt;
}

auto failure_category_from_text(std::string_view text) -> std::optional<failure_category> {
  if (text == "usage_limit") {
    return failure_category::usage_limit;
  }
  if (text == "context_limit") {
    return failure_category::context_limit;
  }
  if (text == "output_limit") {
    return failure_category::output_limit;
  }
  if (text == "tool_failure") {
    return failure_category::tool_failure;
  }
  if (text == "validation") {
    return failure_category::validation;
  }
  if (text == "unknown") {
    return failure_category::unknown;
  }
  return std::nullopt;
}

auto action_kind_from_text(std::string_view text) -> std::optional<action_kind> {
  static constexpr std::array<std::pair<std::string_view, action_kind>, 17> k_table{{
      {"planner", action_kind::planner},
      {"ingestor", action_kind::ingestor},
      {"coder", action_kind::coder},
      {"test_coder", action_kind::test_coder},
      {"reviewer", action_kind::reviewer},
      {"ext_sync", action_kind::ext_sync},
      {"ext_propagate", action_kind::ext_propagate},
      {"orchestrator", action_kind::orchestrator},
      {"resume", action_kind::resume_},
      {"workbench_sync", action_kind::workbench_sync},
      {"spec_draft", action_kind::spec_draft},
      {"claim_check", action_kind::claim_check},
      {"heartbeat", action_kind::heartbeat},
      {"tool_call", action_kind::tool_call},
      {"user_message", action_kind::user_message},
      {"assistant_message", action_kind::assistant_message},
      {"other", action_kind::other},
  }};
  for (auto const& [name, value] : k_table) {
    if (name == text) {
      return value;
    }
  }
  return std::nullopt;
}

auto action_entity_kind_from_text(std::string_view text) -> std::optional<action_entity_kind> {
  static constexpr std::array<std::pair<std::string_view, action_entity_kind>, 7> k_table{{
      {"plan", action_entity_kind::plan},
      {"plan_step", action_entity_kind::plan_step},
      {"task", action_entity_kind::task},
      {"question", action_entity_kind::question},
      {"test_scenario", action_entity_kind::test_scenario},
      {"artifact", action_entity_kind::artifact},
      {"decision", action_entity_kind::decision},
  }};
  for (auto const& [name, value] : k_table) {
    if (name == text) {
      return value;
    }
  }
  return std::nullopt;
}

auto outcome_from_text(std::string_view text) -> std::optional<outcome> {
  if (text == "ok") {
    return outcome::ok;
  }
  if (text == "error") {
    return outcome::error_;
  }
  if (text == "aborted") {
    return outcome::aborted;
  }
  if (text == "timeout") {
    return outcome::timeout;
  }
  return std::nullopt;
}

auto dirty_state_from_text(std::string_view text) -> std::optional<dirty_state> {
  if (text == "clean") {
    return dirty_state::clean;
  }
  if (text == "dirty") {
    return dirty_state::dirty;
  }
  if (text == "unknown") {
    return dirty_state::unknown;
  }
  return std::nullopt;
}

auto probe_default(action_kind kind) -> bool {
  return kind != action_kind::heartbeat && kind != action_kind::tool_call;
}

auto error_name(agent_error err) -> std::string_view {
  switch (err) {
  case agent_error::claim_contention:
    return "ClaimContention";
  case agent_error::claim_not_found:
    return "ClaimNotFound";
  case agent_error::claim_not_active:
    return "ClaimNotActive";
  case agent_error::worktree_not_found:
    return "WorktreeNotFound";
  case agent_error::task_not_found:
    return "TaskNotFound";
  case agent_error::claim_not_on_task:
    return "ClaimNotOnTask";
  case agent_error::illegal_transition:
    return "IllegalTransition";
  case agent_error::unknown_status:
    return "UnknownStatus";
  case agent_error::query_failed:
    return "QueryFailed";
  }
  return "QueryFailed";
}

// =========================================================================
// Acquire
// =========================================================================

auto acquire_claim(db::connection& conn, const acquire_args& args) -> std::expected<claim, agent_error> {
  if (args.worktree_id.has_value()) {
    auto const valid = validate_worktree_id(conn, *args.worktree_id);
    if (!valid) {
      return std::unexpected(valid.error());
    }
  }

  auto const live = has_active_claim(conn, args.kind, args.entity_id);
  if (!live) {
    return std::unexpected(live.error());
  }
  if (*live) {
    if (!args.force) {
      return std::unexpected(agent_error::claim_contention);
    }
    auto const swept = mark_active_stale(conn, args.kind, args.entity_id);
    if (!swept) {
      return std::unexpected(swept.error());
    }
  }

  auto stmt = conn.prepare(std::format("insert into agent_work_claims (\n"
                                       "  claim_token, session_id, entity_kind, entity_id, claim_scope,\n"
                                       "  status, vendor, vendor_session_id, role, model,\n"
                                       "  worktree_id, worktree_path,\n"
                                       "  repo_root, branch, head_sha_at_claim, dirty_at_claim,\n"
                                       "  purpose, base_ref,\n"
                                       "  run_id, stage,\n"
                                       "  lease_expires_at\n"
                                       ") values (\n"
                                       "  lower(hex(randomblob(16))), ?, ?, ?, ?,\n"
                                       "  'active', ?, ?, ?, ?,\n"
                                       "  ?, ?,\n"
                                       "  ?, ?, ?, ?,\n"
                                       "  ?, ?,\n"
                                       "  ?, ?,\n"
                                       "  strftime('%Y-%m-%dT%H:%M:%fZ','now', {})\n"
                                       ")",
                                       seconds_modifier(args.ttl_secs)));
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  bool ok = stmt->bind_int64(1, args.session_id).has_value();
  ok      = ok && stmt->bind_text(2, to_text(args.kind)).has_value();
  ok      = ok && stmt->bind_int64(3, args.entity_id).has_value();
  ok      = ok && stmt->bind_text(4, to_text(args.scope)).has_value();
  ok      = ok && stmt->bind_text(5, args.vendor).has_value();
  ok      = ok && bind_opt_text(*stmt, 6, args.vendor_session_id);
  ok      = ok && bind_opt_text(*stmt, 7, args.role);
  ok      = ok && bind_opt_text(*stmt, 8, args.model);
  ok      = ok && bind_opt_int(*stmt, 9, args.worktree_id);
  ok      = ok && bind_opt_text(*stmt, 10, args.worktree_path);
  ok      = ok && bind_opt_text(*stmt, 11, view(args.loc.repo_root));
  ok      = ok && bind_opt_text(*stmt, 12, view(args.loc.branch));
  ok      = ok && bind_opt_text(*stmt, 13, view(args.loc.head_sha));
  ok      = ok && bind_opt_text(*stmt, 14, claim_dirty_text(args.loc));
  ok      = ok && bind_opt_text(*stmt, 15, args.purpose);
  ok      = ok && bind_opt_text(*stmt, 16, args.base_ref);
  ok      = ok && bind_opt_int(*stmt, 17, args.run_id);
  ok      = ok && bind_opt_text(*stmt, 18, args.stage);
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }

  auto const id = last_insert_rowid(conn);
  if (!id.has_value()) {
    return std::unexpected(agent_error::query_failed);
  }
  return get_claim_by_id(conn, *id);
}

// =========================================================================
// Lease and terminal primitives
// =========================================================================

auto heartbeat_claim(db::connection& conn, std::string_view claim_token, std::optional<std::int64_t> ttl_secs)
    -> std::expected<claim, agent_error> {
  // With an explicit TTL the lease is assigned ABSOLUTELY from `now`.
  // Without one the CURRENT lease length is carried forward, so a
  // heartbeat never shrinks the lease it was sent to preserve. See the
  // interface's doc comment (Planar task 6093).
  auto const lease_expr = ttl_secs.has_value()
                              ? std::format("strftime('%Y-%m-%dT%H:%M:%fZ','now', {})", seconds_modifier(*ttl_secs))
                              : std::string{"strftime('%Y-%m-%dT%H:%M:%fZ','now', '+' || cast(round("
                                            "(julianday(lease_expires_at) - julianday(last_heartbeat_at)) * 86400"
                                            ") as int) || ' seconds')"};
  // `lease_expires_at` must be assigned BEFORE `last_heartbeat_at` is read
  // for the delta; SQLite evaluates every RHS against the OLD row, so the
  // ordering of the SET clauses does not matter here.
  auto stmt = conn.prepare(std::format("update agent_work_claims\n"
                                       "set last_heartbeat_at = {0},\n"
                                       "    lease_expires_at = {1}\n"
                                       "where claim_token = ? and status = 'active'\n"
                                       "  and lease_expires_at >= {0}",
                                       k_now, lease_expr));
  if (!stmt || !stmt->bind_text(1, claim_token) || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  if (changes(conn) == 0) {
    // Distinguish "no such token" from "token exists but is not live":
    // the lookup's own `claim_not_found` propagates, anything found means
    // the guard is what refused.
    auto const existing = get_claim_by_token(conn, claim_token);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    return std::unexpected(agent_error::claim_not_active);
  }
  return get_claim_by_token(conn, claim_token);
}

auto release_claim(db::connection& conn, std::string_view claim_token, claim_status new_status,
                   std::optional<std::string_view> reason, std::optional<failure_category> category)
    -> std::expected<claim, agent_error> {
  if (new_status == claim_status::active) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stmt = conn.prepare(std::format("update agent_work_claims\n"
                                       "set status = ?,\n"
                                       "    released_at = {0},\n"
                                       "    release_reason = ?,\n"
                                       "    failure_category = ?\n"
                                       "where claim_token = ?\n"
                                       "  and status = 'active'\n"
                                       "  and lease_expires_at >= {0}",
                                       k_now));
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  std::optional<std::string_view> category_text;
  if (category.has_value()) {
    category_text = to_text(*category);
  }
  bool ok = stmt->bind_text(1, to_text(new_status)).has_value();
  ok      = ok && bind_opt_text(*stmt, 2, reason);
  ok      = ok && bind_opt_text(*stmt, 3, category_text);
  ok      = ok && stmt->bind_text(4, claim_token).has_value();
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  if (changes(conn) == 0) {
    auto const existing = get_claim_by_token(conn, claim_token);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    return std::unexpected(agent_error::claim_not_active);
  }
  return get_claim_by_token(conn, claim_token);
}

auto abort_claim(db::connection& conn, std::string_view claim_token, std::optional<std::string_view> reason,
                 std::optional<failure_category> category) -> std::expected<claim, agent_error> {
  auto stmt = conn.prepare(std::format("update agent_work_claims\n"
                                       "set status = 'aborted',\n"
                                       "    released_at = {},\n"
                                       "    release_reason = ?,\n"
                                       "    failure_category = ?\n"
                                       "where claim_token = ?",
                                       k_now));
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  std::optional<std::string_view> category_text;
  if (category.has_value()) {
    category_text = to_text(*category);
  }
  bool ok = bind_opt_text(*stmt, 1, reason);
  ok      = ok && bind_opt_text(*stmt, 2, category_text);
  ok      = ok && stmt->bind_text(3, claim_token).has_value();
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  if (changes(conn) == 0) {
    return std::unexpected(agent_error::claim_not_found);
  }
  return get_claim_by_token(conn, claim_token);
}

auto reset_direct_claim_task_after_abort(db::connection& conn, std::int64_t claim_id, std::int64_t task_id)
    -> std::expected<void, agent_error> {
  auto reset = conn.prepare(std::format("update tasks set status = 'todo', updated_at = {0}\n"
                                        "where id = ? and status = 'doing'\n"
                                        "  and exists (\n"
                                        "    select 1 from agent_actions\n"
                                        "    where claim_id = ? and action_kind = 'claim_check'\n"
                                        "  )\n"
                                        "  and not exists (\n"
                                        "    select 1 from agent_work_claims\n"
                                        "    where entity_kind = 'task' and entity_id = ? and status = 'active'\n"
                                        "      and lease_expires_at >= {0}\n"
                                        "  )",
                                        k_now));
  if (!reset || !reset->bind_int64(1, task_id) || !reset->bind_int64(2, claim_id) || !reset->bind_int64(3, task_id) ||
      !reset->step()) {
    return std::unexpected(agent_error::query_failed);
  }

  // The marker delimits the claim's live work interval. Close it in the
  // SAME transaction so no viewer reports work continuing after the claim
  // was force-aborted.
  auto close = conn.prepare(std::format("update agent_actions\n"
                                        "set ended_at = {},\n"
                                        "    outcome = 'aborted'\n"
                                        "where claim_id = ? and action_kind = 'claim_check' and ended_at is null",
                                        k_now));
  if (!close || !close->bind_int64(1, claim_id) || !close->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  return {};
}

auto is_claim_active_unexpired(db::connection& conn, std::string_view claim_token) -> std::expected<bool, agent_error> {
  auto stmt = conn.prepare(std::format("select 1 from agent_work_claims\n"
                                       "where claim_token = ? and status = 'active'\n"
                                       "  and lease_expires_at >= {}",
                                       k_now));
  if (!stmt || !stmt->bind_text(1, claim_token)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

auto has_active_claim_on_task(db::connection& conn, std::int64_t task_id) -> std::expected<bool, agent_error> {
  auto stmt = conn.prepare(std::format("select 1 from agent_work_claims\n"
                                       "where entity_kind = 'task'\n"
                                       "  and entity_id = ?\n"
                                       "  and status = 'active'\n"
                                       "  and lease_expires_at >= {}\n"
                                       "limit 1",
                                       k_now));
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

// =========================================================================
// Reads
// =========================================================================

auto get_claim_by_id(db::connection& conn, std::int64_t id) -> std::expected<claim, agent_error> {
  return fetch_claim(conn, "where id = ?", [id](db::statement& stmt) { return stmt.bind_int64(1, id).has_value(); });
}

auto get_claim_by_token(db::connection& conn, std::string_view token) -> std::expected<claim, agent_error> {
  return fetch_claim(conn, "where claim_token = ?",
                     [token](db::statement& stmt) { return stmt.bind_text(1, token).has_value(); });
}

auto get_task(db::connection& conn, std::int64_t id) -> std::expected<task_row, agent_error> {
  auto stmt = conn.prepare("select id, scope_kind, scope_id, plan_id, parent_task_id, title, body, slug,\n"
                           "       status, priority, next_action, due_at, created_at, updated_at\n"
                           "from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(agent_error::task_not_found);
  }
  task_row row;
  row.id             = stmt->column_int64(0);
  row.scope_kind     = stmt->column_text(1);
  row.scope_id       = opt_int(*stmt, 2);
  row.plan_id        = opt_int(*stmt, 3);
  row.parent_task_id = opt_int(*stmt, 4);
  row.title          = stmt->column_text(5);
  row.body           = opt_text(*stmt, 6);
  row.slug           = opt_text(*stmt, 7);
  row.status         = stmt->column_text(8);
  row.priority       = stmt->column_int64(9);
  row.next_action    = opt_text(*stmt, 10);
  row.due_at         = opt_text(*stmt, 11);
  row.created_at     = stmt->column_text(12);
  row.updated_at     = stmt->column_text(13);
  return row;
}

auto current_task_status(db::connection& conn, std::int64_t id) -> std::expected<std::string, agent_error> {
  auto stmt = conn.prepare("select status from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(agent_error::task_not_found);
  }
  return stmt->column_text(0);
}

auto task_plan_id(db::connection& conn, std::int64_t task_id) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare("select plan_id from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return opt_int(*stmt, 0);
}

// =========================================================================
// Run association
// =========================================================================

auto associate_claim_run(db::connection& conn, std::string_view claim_token, std::int64_t run_id,
                         std::optional<std::string_view> stage) -> std::expected<std::int64_t, agent_error> {
  auto stmt = conn.prepare("update agent_work_claims\n"
                           "set run_id = ?,\n"
                           "    stage = ?\n"
                           "where claim_token = ? and status = 'active'");
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  bool ok = stmt->bind_int64(1, run_id).has_value();
  ok      = ok && bind_opt_text(*stmt, 2, stage);
  ok      = ok && stmt->bind_text(3, claim_token).has_value();
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  return changes(conn);
}

// =========================================================================
// Actions
// =========================================================================

auto start_action(db::connection& conn, const start_action_args& args) -> std::expected<std::int64_t, agent_error> {
  // The table's own CHECK requires both or neither. Refuse here so the
  // caller sees the module's error rather than a driver constraint code.
  if (args.entity.has_value() != args.entity_id.has_value()) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stmt = conn.prepare("insert into agent_actions (\n"
                           "  session_id, session_entry_id, parent_action_id, claim_id,\n"
                           "  action_kind, entity_kind, entity_id,\n"
                           "  vendor, vendor_role, model,\n"
                           "  head_sha, dirty,\n"
                           "  metadata\n"
                           ") values (\n"
                           "  ?, ?, ?, ?,\n"
                           "  ?, ?, ?,\n"
                           "  ?, ?, ?,\n"
                           "  ?, ?,\n"
                           "  ?\n"
                           ")");
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  std::optional<std::string_view> entity_text;
  if (args.entity.has_value()) {
    entity_text = to_text(*args.entity);
  }
  bool ok = stmt->bind_int64(1, args.session_id).has_value();
  ok      = ok && bind_opt_int(*stmt, 2, args.session_entry_id);
  ok      = ok && bind_opt_int(*stmt, 3, args.parent_action_id);
  ok      = ok && bind_opt_int(*stmt, 4, args.claim_id);
  ok      = ok && stmt->bind_text(5, to_text(args.kind)).has_value();
  ok      = ok && bind_opt_text(*stmt, 6, entity_text);
  ok      = ok && bind_opt_int(*stmt, 7, args.entity_id);
  ok      = ok && stmt->bind_text(8, args.vendor).has_value();
  ok      = ok && bind_opt_text(*stmt, 9, args.vendor_role);
  ok      = ok && bind_opt_text(*stmt, 10, args.model);
  ok      = ok && bind_opt_text(*stmt, 11, view(args.loc.head_sha));
  ok      = ok && bind_opt_text(*stmt, 12, action_dirty_text(args.loc));
  ok      = ok && bind_opt_text(*stmt, 13, args.metadata);
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  auto const id = last_insert_rowid(conn);
  if (!id.has_value()) {
    return std::unexpected(agent_error::query_failed);
  }
  return *id;
}

auto end_action(db::connection& conn, std::int64_t id, outcome result, std::optional<std::string_view> summary)
    -> std::expected<void, agent_error> {
  auto stmt = conn.prepare(std::format("update agent_actions\n"
                                       "set ended_at = {},\n"
                                       "    outcome = ?,\n"
                                       "    summary = coalesce(?, summary)\n"
                                       "where id = ? and ended_at is null",
                                       k_now));
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  bool ok = stmt->bind_text(1, to_text(result)).has_value();
  ok      = ok && bind_opt_text(*stmt, 2, summary);
  ok      = ok && stmt->bind_int64(3, id).has_value();
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  return {};
}

auto record_entity_create_action(db::connection& conn, std::int64_t session_id, action_entity_kind entity, std::int64_t entity_id,
                                 std::string_view summary) -> void {
  // Claim id AND vendor in ONE query — the Zig original notes the second
  // round-trip it is avoiding, and the vendor is not optional on the
  // action row.
  //
  // The lease predicate is `>=` against `now`, so a claim whose lease
  // expired is NOT "the latest active claim": it is no claim at all, and
  // the hook goes silent rather than attaching the action to a dead lease.
  auto stmt = conn.prepare(std::format("select id, vendor from agent_work_claims\n"
                                       "where session_id = ?\n"
                                       "  and status = 'active'\n"
                                       "  and lease_expires_at >= {}\n"
                                       "order by claimed_at desc, id desc\n"
                                       "limit 1",
                                       k_now));
  if (!stmt || !stmt->bind_int64(1, session_id)) {
    return;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    // No active claim: operator shell invocation. Silent, per D3.
    return;
  }
  auto const claim_id = stmt->column_int64(0);
  auto const vendor   = stmt->column_text(1);

  auto const action_id = start_action(conn, start_action_args{
                                                .session_id = session_id,
                                                .claim_id   = claim_id,
                                                .kind       = action_kind::other,
                                                .entity     = entity,
                                                .entity_id  = entity_id,
                                                .vendor     = vendor,
                                            });
  if (!action_id) {
    return;
  }
  // Discarded deliberately — see this function's doc comment: the entity's
  // own INSERT has already committed, so a failure here must not surface.
  static_cast<void>(end_action(conn, *action_id, outcome::ok, summary));
}

auto get_action_by_id(db::connection& conn, std::int64_t id) -> std::expected<action, agent_error> {
  auto stmt = conn.prepare(std::string{k_action_columns} + "where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    // zig's getActionById reuses the ClaimNotFound tag for a missing
    // action, and the tag is operator-visible. Preserved.
    return std::unexpected(agent_error::claim_not_found);
  }
  return read_action_row(*stmt);
}

auto close_open_actions_for_claim(db::connection& conn, std::int64_t claim_id, outcome result,
                                  std::optional<std::string_view> summary) -> std::expected<void, agent_error> {
  auto stmt = conn.prepare(std::format("update agent_actions\n"
                                       "set ended_at = {},\n"
                                       "    outcome = ?,\n"
                                       "    summary = coalesce(?, summary)\n"
                                       "where claim_id = ? and ended_at is null",
                                       k_now));
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  bool ok = stmt->bind_text(1, to_text(result)).has_value();
  ok      = ok && bind_opt_text(*stmt, 2, summary);
  ok      = ok && stmt->bind_int64(3, claim_id).has_value();
  if (!ok || !stmt->step()) {
    return std::unexpected(agent_error::query_failed);
  }
  return {};
}

auto latest_open_action_for_claim(db::connection& conn, std::int64_t claim_id) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare("select id from agent_actions where claim_id = ? and ended_at is null\n"
                           "order by started_at desc limit 1");
  if (!stmt || !stmt->bind_int64(1, claim_id)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

// =========================================================================
// Reconcile
// =========================================================================

namespace {

/// @brief Does `candidate` belong to `plan_id`?
///
/// A plan claim matches by identity; a task or plan_step claim matches
/// through its own owning plan. A lookup failure reads as "does not
/// belong" rather than propagating — the sweep must not abort because one
/// referenced row went missing.
/// @param conn The connection.
/// @param candidate The claim.
/// @param plan_id The plan to test against.
/// @return `true` when the claim belongs to that plan.
auto claim_belongs_to_plan(db::connection& conn, const claim& candidate, std::int64_t plan_id) -> bool {
  switch (candidate.kind) {
  case entity_kind::plan:
    return candidate.entity_id == plan_id;
  case entity_kind::task: {
    auto stmt = conn.prepare("select coalesce(plan_id, -1) from tasks where id = ?");
    if (!stmt || !stmt->bind_int64(1, candidate.entity_id)) {
      return false;
    }
    auto stepped = stmt->step();
    if (!stepped || *stepped != db::step_result::row) {
      return false;
    }
    return stmt->column_int64(0) == plan_id;
  }
  case entity_kind::plan_step: {
    auto stmt = conn.prepare("select plan_id from plan_steps where id = ?");
    if (!stmt || !stmt->bind_int64(1, candidate.entity_id)) {
      return false;
    }
    auto stepped = stmt->step();
    if (!stepped || *stepped != db::step_result::row) {
      return false;
    }
    return stmt->column_int64(0) == plan_id;
  }
  }
  return false;
}

} // namespace

auto reconcile_stale(db::connection& conn, const reconcile_policy& policy) -> std::expected<reconcile_result, agent_error> {
  // --- candidates: active claims whose lease passed more than the grace
  // ago. The grace is NEGATED into the modifier, so the default 0 renders
  // '-0 seconds' — a no-op offset, exactly as the oracle does it.
  std::string select_sql =
      std::string{k_claim_columns} + std::format("where status = 'active'\n"
                                                 "  and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', {})",
                                                 seconds_modifier(-policy.stale_after_secs));
  if (policy.session_id.has_value()) {
    select_sql += std::format("\n  and session_id = {}", *policy.session_id);
  }

  std::vector<claim> candidates;
  {
    auto stmt = conn.prepare(select_sql);
    if (!stmt) {
      return std::unexpected(agent_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(agent_error::query_failed);
      }
      if (*stepped != db::step_result::row) {
        break;
      }
      auto row = read_claim_row(*stmt);
      if (!row) {
        return std::unexpected(row.error());
      }
      candidates.push_back(std::move(*row));
    }
  }

  if (policy.plan_id.has_value()) {
    std::erase_if(candidates, [&](const claim& c) { return !claim_belongs_to_plan(conn, c, *policy.plan_id); });
  }

  reconcile_result result;
  if (policy.dry_run) {
    // Writes NOTHING — not the mark-stale, not the task reset, not the
    // orphan sweep. Both counters stay zero even with candidates present.
    result.candidates = std::move(candidates);
    return result;
  }

  std::optional<std::string_view> category_text;
  if (policy.category.has_value()) {
    category_text = to_text(*policy.category);
  }

  if (policy.plan_id.has_value()) {
    // Plan-scoped: one UPDATE per candidate, so claims on other plans are
    // untouched even though they share the expiry predicate.
    for (auto const& candidate : candidates) {
      auto stmt = conn.prepare(std::format("update agent_work_claims\n"
                                           "set status = 'stale',\n"
                                           "    released_at = {},\n"
                                           "    release_reason = 'reconcile: heartbeat expired',\n"
                                           "    failure_category = ?\n"
                                           "where id = ? and status = 'active'",
                                           k_now));
      if (!stmt || !bind_opt_text(*stmt, 1, category_text) || !stmt->bind_int64(2, candidate.id) || !stmt->step()) {
        return std::unexpected(agent_error::query_failed);
      }
    }
  } else {
    std::string update_sql = std::format("update agent_work_claims\n"
                                         "set status = 'stale',\n"
                                         "    released_at = {},\n"
                                         "    release_reason = 'reconcile: heartbeat expired',\n"
                                         "    failure_category = ?\n"
                                         "where status = 'active'\n"
                                         "  and lease_expires_at < strftime('%Y-%m-%dT%H:%M:%fZ','now', {})",
                                         k_now, seconds_modifier(-policy.stale_after_secs));
    if (policy.session_id.has_value()) {
      update_sql += std::format("\n  and session_id = {}", *policy.session_id);
    }
    auto stmt = conn.prepare(update_sql);
    if (!stmt || !bind_opt_text(*stmt, 1, category_text) || !stmt->step()) {
      return std::unexpected(agent_error::query_failed);
    }
  }
  // Counted from the CANDIDATE LIST, not from `changes()`. See the
  // interface's doc comment.
  result.claims_marked_stale = static_cast<std::int64_t>(candidates.size());

  // --- return tasks to `todo` where THIS claim owns the transition.
  // The evidence is any action row on the claim (pull's role action or a
  // direct claim's `claim_check`); a `--no-transition` claim has none and
  // so cannot reset a status it never set. The `not exists` guard keeps a
  // replacement claim's task from being yanked away.
  for (auto const& candidate : candidates) {
    if (candidate.kind != entity_kind::task) {
      continue;
    }
    auto reset = conn.prepare(std::format("update tasks set status = 'todo', updated_at = {0}\n"
                                          "where id = ? and status = 'doing'\n"
                                          "  and exists (select 1 from agent_actions where claim_id = ?)\n"
                                          "  and not exists (\n"
                                          "    select 1 from agent_work_claims\n"
                                          "    where entity_kind = 'task' and entity_id = ? and status = 'active'\n"
                                          "      and lease_expires_at >= {0}\n"
                                          "  )",
                                          k_now));
    if (!reset || !reset->bind_int64(1, candidate.entity_id) || !reset->bind_int64(2, candidate.id) ||
        !reset->bind_int64(3, candidate.entity_id) || !reset->step()) {
      return std::unexpected(agent_error::query_failed);
    }

    auto close = conn.prepare(std::format("update agent_actions\n"
                                          "set ended_at = {},\n"
                                          "    outcome = 'aborted'\n"
                                          "where claim_id = ? and action_kind = 'claim_check' and ended_at is null",
                                          k_now));
    if (!close || !close->bind_int64(1, candidate.id) || !close->step()) {
      return std::unexpected(agent_error::query_failed);
    }
    result.actions_closed += changes(conn);
  }

  // --- orphaned actions: still open, but their owning session has ended.
  // Closed with the SESSION's `ended_at`, not with now, so the timeline
  // does not claim work continued past the session that was doing it.
  std::string orphan_sql = std::format("update agent_actions\n"
                                       "set ended_at = (select ended_at from sessions where id = agent_actions.session_id),\n"
                                       "    outcome = 'aborted'\n"
                                       "where ended_at is null\n");
  if (policy.session_id.has_value()) {
    orphan_sql += std::format("  and session_id = {}\n", *policy.session_id);
  }
  if (policy.plan_id.has_value()) {
    // Plan-scoped sweeps only close actions whose CLAIM names an entity in
    // that plan. An action with no claim is session-level housekeeping and
    // has no plan affiliation to filter on, so it is left alone.
    orphan_sql += std::format(
        "  and exists (select 1 from sessions s where s.id = agent_actions.session_id and s.ended_at is not null)\n"
        "  and claim_id is not null\n"
        "  and exists (\n"
        "    select 1 from agent_work_claims c\n"
        "    where c.id = agent_actions.claim_id\n"
        "      and (\n"
        "        (c.entity_kind = 'plan' and c.entity_id = {0})\n"
        "        or (c.entity_kind = 'task' and exists (select 1 from tasks t where t.id = c.entity_id and t.plan_id = {0}))\n"
        "      )\n"
        "  )",
        *policy.plan_id);
  } else {
    orphan_sql += "  and exists (\n"
                  "    select 1 from sessions s\n"
                  "    where s.id = agent_actions.session_id and s.ended_at is not null\n"
                  "  )";
  }
  {
    auto stmt = conn.prepare(orphan_sql);
    if (!stmt || !stmt->step()) {
      return std::unexpected(agent_error::query_failed);
    }
    result.actions_closed += changes(conn);
  }

  result.candidates = std::move(candidates);
  return result;
}

auto pid_alive(std::int64_t pid) -> bool {
  if (pid <= 0) {
    return false;
  }
  if (::kill(static_cast<::pid_t>(pid), 0) == 0) {
    return true;
  }
  // ESRCH is the ONLY errno that means "gone". EPERM means the process
  // exists under another owner, and any other errno is a question we
  // cannot answer — both read as alive, because the conservative direction
  // here is to leave a run running rather than abandon one that is not.
  return errno != ESRCH;
}

auto reconcile_runs(db::connection& conn, bool dry_run, std::optional<std::int64_t> plan_id)
    -> std::expected<reconcile_runs_result, agent_error> {
  std::string select_sql = "select id, run_identifier, pid, plan_id from workflow_runs where status = 'running'";
  if (plan_id.has_value()) {
    select_sql += std::format(" and plan_id = {}", *plan_id);
  }

  reconcile_runs_result result;
  {
    auto stmt = conn.prepare(select_sql);
    if (!stmt) {
      return std::unexpected(agent_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(agent_error::query_failed);
      }
      if (*stepped != db::step_result::row) {
        break;
      }
      run_candidate row{.id             = stmt->column_int64(0),
                        .run_identifier = stmt->column_text(1),
                        .pid            = stmt->column_int64(2),
                        .plan_id        = opt_int(*stmt, 3)};
      if (!pid_alive(row.pid)) {
        result.candidates.push_back(std::move(row));
      }
    }
  }

  if (dry_run) {
    return result;
  }

  for (auto const& candidate : result.candidates) {
    auto stmt = conn.prepare(std::format("update workflow_runs\n"
                                         "set status = 'abandoned', ended_at = {}\n"
                                         "where id = ? and status = 'running'",
                                         k_now));
    if (!stmt || !stmt->bind_int64(1, candidate.id) || !stmt->step()) {
      return std::unexpected(agent_error::query_failed);
    }
    // Counted per ATTEMPT, not per row actually matched — zig's behavior.
    ++result.abandoned;
  }
  return result;
}

// =========================================================================
// Read paths — the display half (task 6120)
// =========================================================================

namespace {

/// @brief Run `sql` (already composed onto `k_claim_columns`) and decode
/// every row.
/// @param conn The connection.
/// @param sql The complete statement.
/// @param bind Applies the statement's bound parameters; may be empty.
/// @return The rows, or `query_failed`.
auto collect_claims(db::connection& conn, std::string_view sql, const std::function<bool(db::statement&)>& bind)
    -> std::expected<std::vector<claim>, agent_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  if (bind && !bind(*stmt)) {
    return std::unexpected(agent_error::query_failed);
  }
  std::vector<claim> rows;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      return rows;
    }
    auto row = read_claim_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
}

/// @brief Run `sql` (already composed onto `k_action_columns`) and decode
/// every row.
/// @param conn The connection.
/// @param sql The complete statement.
/// @param bind Applies the statement's bound parameters; may be empty.
/// @return The rows, or `query_failed`.
auto collect_actions(db::connection& conn, std::string_view sql, const std::function<bool(db::statement&)>& bind)
    -> std::expected<std::vector<action>, agent_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  if (bind && !bind(*stmt)) {
    return std::unexpected(agent_error::query_failed);
  }
  std::vector<action> rows;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      return rows;
    }
    auto row = read_action_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    rows.push_back(std::move(*row));
  }
}

/// @brief The lease-liveness half of the `active` predicate, spelled once.
auto lease_live() -> std::string {
  return std::format("lease_expires_at >= {}", k_now);
}

/// @brief The `stale` predicate, spelled once: reconcile-marked OR expired.
auto stale_predicate() -> std::string {
  return std::format("status = 'stale'\n   or (status = 'active' and lease_expires_at < {})", k_now);
}

/// @brief Read a single integer out of a one-parameter query.
/// @param conn The connection.
/// @param sql The statement.
/// @param param The value to bind at position 1.
/// @return The value, or unset when the query failed or matched no row.
auto int_query_1(db::connection& conn, std::string_view sql, std::int64_t param) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, param)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief The `plan_id` a task rolls up to.
///
/// A missing task and a NULL `plan_id` both yield `-1`, zig's sentinel. A
/// real plan id is always >= 1, so the sentinel can never collide with one.
/// @param conn The connection.
/// @param task_id The task's row id.
/// @return The plan id, or `-1`.
auto task_plan(db::connection& conn, std::int64_t task_id) -> std::int64_t {
  return int_query_1(conn, "select coalesce(plan_id, -1) from tasks where id = ?", task_id).value_or(-1);
}

/// @brief The `plan_id` a plan_step rolls up to.
/// @param conn The connection.
/// @param step_id The step's row id.
/// @return The plan id, or `-1`.
auto plan_step_plan(db::connection& conn, std::int64_t step_id) -> std::int64_t {
  return int_query_1(conn, "select plan_id from plan_steps where id = ?", step_id).value_or(-1);
}

/// @brief Mark each node that has no later sibling.
///
/// Port of zig's `annotateLastSibling`, INCLUDING its treatment of roots:
/// two roots are siblings only when they share a `session_id`. That makes
/// the last root of every session render with `└──`'s depth-0 equivalent
/// (no prefix) rather than the forest as a whole having one last root. The
/// walk is ordered by `id asc`, so "later" is "further along the vector".
/// @param nodes The flattened walk, mutated in place.
auto annotate_last_sibling(std::span<forest_node> nodes) -> void {
  for (std::size_t i = nodes.size(); i > 0; --i) {
    auto&      self        = nodes[i - 1];
    bool       found_later = false;
    auto const rest        = nodes.subspan(i);
    for (auto const& later : rest) {
      bool const same_parent = self.parent_action_id.has_value()
                                   ? (later.parent_action_id.has_value() && *later.parent_action_id == *self.parent_action_id)
                                   : (!later.parent_action_id.has_value() && later.session_id == self.session_id);
      if (same_parent) {
        found_later = true;
        break;
      }
    }
    self.is_last_sibling = !found_later;
  }
}

} // namespace

auto resolve_claim_scope(db::connection& conn, const claim& value) -> claim_scope_info {
  // Not `constexpr`: the struct holds a `std::string`, so it is not a
  // literal type. The default member initialisers ARE the degraded form.
  claim_scope_info const k_unknown{};

  std::string_view scope_sql;
  switch (value.kind) {
  case entity_kind::plan:
    scope_sql = "select scope_kind, scope_id from plans where id = ?";
    break;
  case entity_kind::task:
    scope_sql = "select scope_kind, scope_id from tasks where id = ?";
    break;
  case entity_kind::plan_step:
    // plan_steps carry no scope columns of their own (00003_work_items);
    // they inherit the parent plan's.
    scope_sql = "select p.scope_kind, p.scope_id from plan_steps ps\n"
                "  join plans p on ps.plan_id = p.id where ps.id = ?";
    break;
  }

  auto stmt = conn.prepare(scope_sql);
  if (!stmt || !stmt->bind_int64(1, value.entity_id)) {
    return k_unknown;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return k_unknown;
  }

  auto const  raw = stmt->column_text(0);
  std::string kind;
  if (raw == "global" || raw == "association" || raw == "repo") {
    kind = raw;
  } else {
    kind = "?";
  }
  if (kind == "global") {
    return claim_scope_info{.kind = "global", .slug = std::nullopt};
  }
  auto const scope_id = opt_int(*stmt, 1);
  if (!scope_id.has_value()) {
    return claim_scope_info{.kind = kind, .slug = std::nullopt};
  }

  std::string_view const slug_sql =
      kind == "association" ? "select slug from associations where id = ?" : "select slug from projects where id = ?";
  auto slug_stmt = conn.prepare(slug_sql);
  if (!slug_stmt || !slug_stmt->bind_int64(1, *scope_id)) {
    return claim_scope_info{.kind = kind, .slug = std::nullopt};
  }
  auto slug_stepped = slug_stmt->step();
  if (!slug_stepped || *slug_stepped != db::step_result::row) {
    return claim_scope_info{.kind = kind, .slug = std::nullopt};
  }
  return claim_scope_info{.kind = kind, .slug = slug_stmt->column_text(0)};
}

auto parse_claim_status_filter(std::optional<std::string_view> text) -> claim_status_filter {
  if (!text.has_value()) {
    return claim_status_filter::active;
  }
  if (*text == "stale") {
    return claim_status_filter::stale;
  }
  if (*text == "all") {
    return claim_status_filter::all;
  }
  // "active" AND every unrecognised value. See the declaration's note.
  return claim_status_filter::active;
}

auto list_claims(db::connection& conn, claim_status_filter filter) -> std::expected<std::vector<claim>, agent_error> {
  std::string sql{k_claim_columns};
  switch (filter) {
  case claim_status_filter::active:
    sql += std::format("where status = 'active'\n  and {}\norder by claimed_at desc", lease_live());
    break;
  case claim_status_filter::stale:
    sql += std::format("where {}\norder by claimed_at desc", stale_predicate());
    break;
  case claim_status_filter::all:
    sql += "order by claimed_at desc";
    break;
  }
  return collect_claims(conn, sql, {});
}

auto parse_ps_sort(std::optional<std::string_view> text) -> std::optional<ps_sort> {
  if (!text.has_value() || *text == "heartbeat") {
    return ps_sort::heartbeat;
  }
  if (*text == "lease") {
    return ps_sort::lease;
  }
  return std::nullopt;
}

auto list_active_claims_sorted(db::connection& conn, ps_sort sort) -> std::expected<std::vector<claim>, agent_error> {
  std::string sql{k_claim_columns};
  // No lease predicate. See the declaration — this is zig's behavior and
  // the reason a stale claim can appear twice under `ps --stale`.
  sql += "where status = 'active'\n";
  if (sort == ps_sort::heartbeat) {
    // SQLite has no NULLS LAST; the CASE expression is the idiom.
    sql += "order by case when last_heartbeat_at is null then 1 else 0 end asc,\n"
           "         last_heartbeat_at desc, id desc";
  } else {
    sql += "order by claimed_at desc";
  }
  return collect_claims(conn, sql, {});
}

auto list_stale_claims(db::connection& conn) -> std::expected<std::vector<claim>, agent_error> {
  return collect_claims(conn, std::string{k_claim_columns} + std::format("where {}\norder by claimed_at desc", stale_predicate()),
                        {});
}

auto list_actions(db::connection& conn, std::int64_t limit) -> std::expected<std::vector<action>, agent_error> {
  return collect_actions(conn, std::string{k_action_columns} + std::format("order by started_at desc, id desc\nlimit {}", limit),
                         {});
}

auto latest_action_for_claim(db::connection& conn, std::int64_t claim_id) -> std::expected<std::optional<action>, agent_error> {
  auto rows =
      collect_actions(conn, std::string{k_action_columns} + "where claim_id = ?\norder by started_at desc, id desc\nlimit 1",
                      [claim_id](db::statement& stmt) { return stmt.bind_int64(1, claim_id).has_value(); });
  if (!rows) {
    return std::unexpected(rows.error());
  }
  if (rows->empty()) {
    return std::optional<action>{};
  }
  return std::optional<action>{std::move(rows->front())};
}

auto list_actions_by_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error> {
  return collect_actions(
      conn,
      std::string{k_action_columns} +
          std::format("where entity_kind = ? and entity_id = ?\norder by started_at asc, id asc\nlimit {}", limit),
      [entity_kind, entity_id](db::statement& stmt) {
        return stmt.bind_text(1, entity_kind).has_value() && stmt.bind_int64(2, entity_id).has_value();
      });
}

auto list_actions_by_session(db::connection& conn, std::int64_t session_id, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error> {
  return collect_actions(
      conn, std::string{k_action_columns} + std::format("where session_id = ?\norder by started_at asc, id asc\nlimit {}", limit),
      [session_id](db::statement& stmt) { return stmt.bind_int64(1, session_id).has_value(); });
}

auto list_actions_by_claim_token(db::connection& conn, std::string_view token, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error> {
  return collect_actions(conn,
                         std::string{k_action_columns} +
                             std::format("where claim_id = (select id from agent_work_claims where claim_token = ?)\n"
                                         "order by started_at asc, id asc\nlimit {}",
                                         limit),
                         [token](db::statement& stmt) { return stmt.bind_text(1, token).has_value(); });
}

auto list_claims_by_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<claim>, agent_error> {
  return collect_claims(conn, std::string{k_claim_columns} + "where entity_kind = ? and entity_id = ?\norder by claimed_at asc",
                        [entity_kind, entity_id](db::statement& stmt) {
                          return stmt.bind_text(1, entity_kind).has_value() && stmt.bind_int64(2, entity_id).has_value();
                        });
}

auto list_claims_by_session(db::connection& conn, std::int64_t session_id) -> std::expected<std::vector<claim>, agent_error> {
  return collect_claims(conn, std::string{k_claim_columns} + "where session_id = ?\norder by claimed_at asc",
                        [session_id](db::statement& stmt) { return stmt.bind_int64(1, session_id).has_value(); });
}

auto list_claims_by_token(db::connection& conn, std::string_view token) -> std::expected<std::vector<claim>, agent_error> {
  return collect_claims(conn, std::string{k_claim_columns} + "where claim_token = ?",
                        [token](db::statement& stmt) { return stmt.bind_text(1, token).has_value(); });
}

auto recent_actions_for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t limit)
    -> std::expected<std::vector<action>, agent_error> {
  // DESC on the COALESCE, not on `started_at`. See the header: the
  // neighbouring `list_actions_by_entity` sorts the opposite way on a
  // different column, and the two are not interchangeable.
  return collect_actions(conn,
                         std::string{k_action_columns} +
                             std::format("where entity_kind = ? and entity_id = ?\n"
                                         "order by coalesce(ended_at, started_at) desc, id desc\nlimit {}",
                                         limit),
                         [entity_kind, entity_id](db::statement& stmt) {
                           return stmt.bind_text(1, entity_kind).has_value() && stmt.bind_int64(2, entity_id).has_value();
                         });
}

auto claim_transitions_for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t limit)
    -> std::expected<std::vector<claim>, agent_error> {
  return collect_claims(conn,
                        std::string{k_claim_columns} +
                            std::format("where entity_kind = ? and entity_id = ?\n"
                                        "order by coalesce(released_at, claimed_at) desc, id desc\nlimit {}",
                                        limit),
                        [entity_kind, entity_id](db::statement& stmt) {
                          return stmt.bind_text(1, entity_kind).has_value() && stmt.bind_int64(2, entity_id).has_value();
                        });
}

auto claim_belongs_to_plan(db::connection& conn, entity_kind kind, std::int64_t entity_id, std::int64_t plan_id) -> bool {
  switch (kind) {
  case entity_kind::plan:
    return entity_id == plan_id;
  case entity_kind::task:
    return task_plan(conn, entity_id) == plan_id;
  case entity_kind::plan_step:
    return plan_step_plan(conn, entity_id) == plan_id;
  }
  return false;
}

auto action_belongs_to_plan(db::connection& conn, action_entity_kind kind, std::int64_t entity_id, std::int64_t plan_id) -> bool {
  switch (kind) {
  case action_entity_kind::plan:
    return entity_id == plan_id;
  case action_entity_kind::task:
    return task_plan(conn, entity_id) == plan_id;
  case action_entity_kind::plan_step:
    return plan_step_plan(conn, entity_id) == plan_id;
  case action_entity_kind::question:
  case action_entity_kind::test_scenario:
  case action_entity_kind::artifact:
  case action_entity_kind::decision:
    // No plan link in the schema. See the declaration.
    return false;
  }
  return false;
}

auto collect_plan_activity(db::connection& conn, std::int64_t plan_id) -> std::expected<plan_activity, agent_error> {
  plan_activity result;

  {
    auto stmt = conn.prepare(std::format("select count(*) from agent_work_claims c\n"
                                         "where c.status = 'active'\n"
                                         "  and c.{}\n"
                                         "  and (\n"
                                         "    (c.entity_kind = 'plan' and c.entity_id = ?)\n"
                                         "    or (c.entity_kind = 'task' and exists (\n"
                                         "         select 1 from tasks t where t.id = c.entity_id and t.plan_id = ?))\n"
                                         "  )",
                                         lease_live()));
    if (!stmt || !stmt->bind_int64(1, plan_id) || !stmt->bind_int64(2, plan_id)) {
      return std::unexpected(agent_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      result.active_claims = stmt->column_int64(0);
    }
  }

  {
    auto stmt = conn.prepare("select count(*) from agent_actions a\n"
                             "where a.ended_at is null\n"
                             "  and (\n"
                             "    (a.entity_kind = 'plan' and a.entity_id = ?)\n"
                             "    or (a.entity_kind = 'task' and exists (\n"
                             "         select 1 from tasks t where t.id = a.entity_id and t.plan_id = ?))\n"
                             "  )");
    if (!stmt || !stmt->bind_int64(1, plan_id) || !stmt->bind_int64(2, plan_id)) {
      return std::unexpected(agent_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      result.active_actions = stmt->column_int64(0);
    }
  }

  {
    // `max(a, b, c)` with several arguments is SQLite's SCALAR max, not the
    // aggregate; the outer `max(event_at)` over the union IS the aggregate.
    // Both spellings appear here deliberately and are zig's.
    auto stmt = conn.prepare("select max(event_at) from (\n"
                             "  select max(claimed_at, last_heartbeat_at, coalesce(released_at, claimed_at)) as event_at\n"
                             "  from agent_work_claims c\n"
                             "  where (c.entity_kind = 'plan' and c.entity_id = ?)\n"
                             "     or (c.entity_kind = 'task' and exists (\n"
                             "         select 1 from tasks t where t.id = c.entity_id and t.plan_id = ?))\n"
                             "  union all\n"
                             "  select coalesce(ended_at, started_at) as event_at\n"
                             "  from agent_actions a\n"
                             "  where (a.entity_kind = 'plan' and a.entity_id = ?)\n"
                             "     or (a.entity_kind = 'task' and exists (\n"
                             "         select 1 from tasks t where t.id = a.entity_id and t.plan_id = ?))\n"
                             ")");
    if (!stmt) {
      return std::unexpected(agent_error::query_failed);
    }
    for (int i = 1; i <= 4; ++i) {
      if (!stmt->bind_int64(i, plan_id)) {
        return std::unexpected(agent_error::query_failed);
      }
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      result.last_event_at = opt_text(*stmt, 0);
    }
  }

  return result;
}

auto session_exists(db::connection& conn, std::int64_t session_id) -> std::expected<bool, agent_error> {
  auto stmt = conn.prepare("select 1 from sessions where id = ? limit 1");
  if (!stmt || !stmt->bind_int64(1, session_id)) {
    return std::unexpected(agent_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(agent_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

auto walk_action_forest(db::connection& conn, std::optional<std::int64_t> root_session_id)
    -> std::expected<std::vector<forest_node>, agent_error> {
  std::string const anchor_filter =
      root_session_id.has_value() ? "where parent_action_id is null and session_id = ?" : "where parent_action_id is null";
  auto const sql = std::format("with recursive tree(action_id, parent_action_id, session_id, claim_id, depth) as (\n"
                               "  select id, parent_action_id, session_id, claim_id, 0\n"
                               "  from agent_actions\n"
                               "  {}\n"
                               "  union all\n"
                               "  select a.id, a.parent_action_id, a.session_id, a.claim_id, t.depth + 1\n"
                               "  from agent_actions a\n"
                               "  join tree t on a.parent_action_id = t.action_id\n"
                               ")\n"
                               "select action_id, parent_action_id, session_id, claim_id, depth\n"
                               "from tree\n"
                               "order by action_id asc",
                               anchor_filter);

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  if (root_session_id.has_value() && !stmt->bind_int64(1, *root_session_id)) {
    return std::unexpected(agent_error::query_failed);
  }

  std::vector<forest_node> nodes;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      break;
    }
    nodes.push_back(forest_node{.id               = stmt->column_int64(0),
                                .parent_action_id = opt_int(*stmt, 1),
                                .session_id       = stmt->column_int64(2),
                                .claim_id         = opt_int(*stmt, 3),
                                .depth            = stmt->column_int64(4),
                                .is_last_sibling  = false});
  }
  annotate_last_sibling(nodes);
  return nodes;
}

namespace {

/// @brief The correlated sub-select that finds the claim covering a task.
///
/// Composed twice with a different WHERE tail — once for the LIVE claim and
/// once for the STALE one — because the two differ only in that predicate
/// and a copied 12-line sub-select is exactly the drift `k_claim_columns`
/// exists to prevent one file over.
///
/// The `left join` onto `task_ancestors` is what turns a claim on an
/// ancestor plan (or one of its steps) into coverage of a descendant task;
/// `ta.task_id is not null` is the "this ancestor claim reaches me" test,
/// and `ta.depth` carries the distance the ORDER BY reads as "nearest".
/// @param predicate The liveness tail, already spelled.
/// @return The sub-select, parenthesised and ready to embed.
auto covering_claim_select(std::string_view predicate) -> std::string {
  return std::format("(\n"
                     "  select c.id\n"
                     "  from agent_work_claims c\n"
                     "  left join plan_steps ps on c.entity_kind = 'plan_step' and ps.id = c.entity_id\n"
                     "  left join task_ancestors ta on ta.task_id = t.id and ta.plan_id =\n"
                     "    case when c.entity_kind = 'plan' then c.entity_id\n"
                     "         when c.entity_kind = 'plan_step' then ps.plan_id end\n"
                     "  where ((c.entity_kind = 'task' and c.entity_id = t.id)\n"
                     "      or (c.entity_kind in ('plan','plan_step') and ta.task_id is not null))\n"
                     "    and {}\n"
                     "  order by case c.entity_kind when 'task' then 0 when 'plan_step' then 1 else 2 end,\n"
                     "           coalesce(ta.depth, 0), c.id desc\n"
                     "  limit 1\n"
                     ")",
                     predicate);
}

} // namespace

auto next_work(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<next_work_row>, agent_error> {
  // The two liveness predicates are spelled OUT here rather than reusing
  // this file's `lease_live()` / `stale_predicate()` helpers, and the
  // reason is not style: both helpers emit UNQUALIFIED `status` /
  // `lease_expires_at`, which is unambiguous in the single-table reads they
  // were written for and ambiguous here — the sub-select joins
  // `plan_steps`, which has a `status` column of its own. `stale_predicate`
  // is also unparenthesised at the top level (its callers put it directly
  // after `where`), so composing it under an `and` would silently invert
  // the precedence. Qualified and parenthesised, exactly as the oracle
  // writes it.
  auto const live_tail = std::format("c.status = 'active'\n      and c.lease_expires_at >= {}", k_now);
  auto const stale_tail =
      std::format("(c.status = 'stale'\n        or (c.status = 'active' and c.lease_expires_at < {}))", k_now);

  auto const sql = std::format("with recursive\n"
                               "plan_tree(id) as (\n"
                               "  select id from plans where id = ?\n"
                               "  union all\n"
                               "  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id\n"
                               "),\n"
                               "task_ancestors(task_id, plan_id, depth) as (\n"
                               "  select t.id, t.plan_id, 0\n"
                               "  from tasks t join plan_tree pt on pt.id = t.plan_id\n"
                               "  union all\n"
                               "  select ta.task_id, p.parent_plan_id, ta.depth + 1\n"
                               "  from task_ancestors ta\n"
                               "  join plans p on p.id = ta.plan_id\n"
                               "  where p.parent_plan_id is not null\n"
                               ")\n"
                               "select t.id, t.title, t.status, t.priority,\n"
                               "       {} as active_claim_id,\n"
                               "       {} as stale_claim_id\n"
                               "from tasks t\n"
                               "join plan_tree pt on pt.id = t.plan_id\n"
                               "order by t.priority asc, t.id asc",
                               covering_claim_select(live_tail), covering_claim_select(stale_tail));

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(agent_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id)) {
    return std::unexpected(agent_error::query_failed);
  }

  std::vector<next_work_row> rows;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(agent_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      break;
    }

    auto const status    = std::string{stmt->column_text(2)};
    auto const active_id = opt_int(*stmt, 4);
    auto const stale_id  = opt_int(*stmt, 5);

    // The ladder. `blocked` first and unconditionally — see the header.
    next_work_bucket     bucket{};
    std::optional<claim> covering;
    if (status == "blocked") {
      bucket = next_work_bucket::blocked;
    } else if (active_id.has_value()) {
      bucket    = next_work_bucket::claimed;
      auto held = get_claim_by_id(conn, *active_id);
      if (!held) {
        return std::unexpected(held.error());
      }
      covering = std::move(*held);
    } else if (stale_id.has_value()) {
      bucket    = next_work_bucket::stale;
      auto held = get_claim_by_id(conn, *stale_id);
      if (!held) {
        return std::unexpected(held.error());
      }
      covering = std::move(*held);
    } else if (status == "todo" || status == "doing") {
      // `doing` with no live claim: a reconciled-away claim's leftover.
      // Surfaced as available so a fresh pull picks it up.
      bucket = next_work_bucket::available;
    } else {
      // done / cancelled: in NO bucket at all.
      continue;
    }

    rows.push_back(next_work_row{.bucket   = bucket,
                                 .task_id  = stmt->column_int64(0),
                                 .title    = std::string{stmt->column_text(1)},
                                 .status   = status,
                                 .priority = stmt->column_int64(3),
                                 .covering = std::move(covering)});
  }
  return rows;
}

} // namespace planar::engine::runtime::agentactivity
