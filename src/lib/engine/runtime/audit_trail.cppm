/// @file audit_trail.cppm
/// @brief `planar.engine.runtime.audit_trail` — the READ half of the audit
/// plane (plan 996, task 6090).
///
/// Behavior-preserving port (D2) of zig/src/engine/runtime/audit_trail.zig:
/// `forEntity`, `forEntityWithLinks`, `forEntityGrep`, and
/// `sessionTimeline`. Every function here is a pure read; nothing in this
/// module ever writes `audit_log`. Rows get INTO `audit_log` through
/// `planar.policy`'s record path, which is a different module with a
/// different job.
///
/// ## This is NOT `planar.policy.audit`
///
/// `src/lib/policy/audit.cppm` is audit POLICY — the write-side decision
/// about what a mutation records. It exports nothing this module calls and
/// this module exports nothing it calls. The Zig tree draws the same line
/// (`engine/policy.zig` vs `engine/runtime/audit_trail.zig`); the names are
/// close enough that conflating them is easy, so: the write path existed
/// before this port and the READ path had no C++ counterpart at all.
///
/// ## Why this bucket
///
/// `engine_runtime` and not a new `engine_audit`. `session_timeline` reads
/// `sessions` and `session_entries` — the two tables
/// `planar.engine.runtime.session` already owns — so a separate bucket
/// would either duplicate that vocabulary or need an `engine_* ->
/// engine_*` edge, which D15/D18 forbid and `cmake/architecture.cmake`
/// FATALs on at configure time. The Zig original sits at
/// `engine/runtime/audit_trail.zig`, a sibling of `engine/runtime/
/// session.zig`, for the same reason. The module needs `db` and nothing
/// else, so no D19 extraction was required and none was invented.
///
/// ## `session_timeline` reads its own rows rather than calling `session`
///
/// It selects four columns straight out of `sessions` instead of reaching
/// for `session::get_by_id`, which returns a twelve-column `session`
/// struct. That mirrors the oracle exactly (`select vendor, task_id,
/// started_at, ended_at from sessions where id = ?`), and it matters for
/// more than efficiency: `get_by_id` maps a missing row to
/// `session_error::not_found` from a DIFFERENT error enum, so routing
/// through it would force a cross-enum translation for no gain. The
/// entries query likewise reads `ordinal, prefix, body` where
/// `session::list_entries_for_session` reads six columns — the three this
/// module drops are ones `audit session` never renders.
///
/// ## What is NOT here
///
/// The `external_links` / `sync_events` half of `planar audit trail
/// <link-id>`. The Zig module's own header defers it the same way, and the
/// leaf that would consume it is not wired in this cycle. Named rather
/// than silently dropped.
module;

export module planar.engine.runtime.audit_trail;

import std;
import planar.db;

namespace planar::engine::runtime::audit_trail {

/// @brief One `audit_log` row, as every query in this module returns it.
///
/// Field order mirrors the oracle's `AuditEntry` and the `select` list
/// behind it, so a JSON renderer that walks the struct emits the oracle's
/// key order.
export struct audit_entry {
  std::int64_t               id = 0;        ///< The `audit_log` row id.
  std::string                verb;          ///< The verb that recorded the row.
  std::string                entity_kind;   ///< The mutated entity's kind.
  std::int64_t               entity_id = 0; ///< The mutated entity's row id.
  std::optional<std::string> actor;         ///< Who performed it, when recorded.
  std::optional<std::string> scope;         ///< The scope label, when recorded.
  std::optional<std::string> summary;       ///< The free-text summary, when recorded.
  std::string                recorded_at;   ///< The ISO-8601 timestamp.
};

/// @brief One `session_entries` row, as `session_timeline` returns it.
export struct session_entry {
  std::int64_t ordinal = 0; ///< Position within the session, 1-based.
  std::string  prefix;      ///< The entry's category tag (`note`, `command`, …).
  std::string  body;        ///< The entry text.
};

/// @brief A session header plus its full ordered entry list.
export struct session_timeline_result {
  std::int64_t                session_id = 0; ///< The `sessions` row id (echoed from the argument).
  std::string                 vendor;         ///< The recording vendor.
  std::optional<std::int64_t> task_id;        ///< The task the session is bound to, when bound.
  std::string                 started_at;     ///< The ISO-8601 start timestamp.
  std::optional<std::string>  ended_at;       ///< The end timestamp; unset while the session is active.
  std::vector<session_entry>  entries;        ///< The entries, ordered by `ordinal`.
};

/// @brief Error surface for this module.
export enum class audit_error : std::uint8_t {
  query_failed, ///< An underlying SQL statement failed.
  not_found,    ///< `session_timeline` was given an id no `sessions` row has.
};

/// @brief Every `audit_log` row for one entity, oldest first.
///
/// Ordered by `id`, not by `recorded_at` — the oracle's `order by id`. The
/// two can disagree when several rows share a timestamp, and `id` is the
/// stable one.
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind to match (`task`, `plan`, …).
/// @param entity_id The entity row id to match.
/// @return The rows in id order (possibly empty — an entity with no audit
/// history is an answer, not an error), or `audit_error::query_failed`.
export auto for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<audit_entry>, audit_error>;

/// @brief `for_entity`, widened by ONE hop through `entity_links` in
/// EITHER direction.
///
/// Returns the entity's own rows plus the rows of every entity reachable
/// from it by a single `entity_links` edge — following `from -> to` and
/// `to -> from` both. Serves `audit trail <task-id>`, where the useful
/// history includes the decisions and questions linked to the task, not
/// only the task's own mutations.
///
/// One hop, not transitive closure: the oracle's subquery has no recursive
/// CTE, so a decision linked to a question linked to the task does NOT
/// appear. Reproduced, not extended.
///
/// The result is a single ordered-by-id list with no de-duplication clause
/// beyond what `in (...)` already gives — a row is reachable at most once
/// because the match is on `(entity_kind, entity_id)`, not on the edge.
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind to start from.
/// @param entity_id The entity row id to start from.
/// @return The rows in id order (possibly empty), or `audit_error::query_failed`.
export auto for_entity_with_links(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<audit_entry>, audit_error>;

/// @brief `for_entity`, narrowed to rows whose `summary` CONTAINS
/// `pattern`.
///
/// The match is SQL `like '%' || pattern || '%'`, which means two things
/// worth stating because neither is obvious from the name: it is
/// case-insensitive for ASCII (SQLite's `like` default), and `%` or `_`
/// inside `pattern` are wildcards, not literals — there is no `escape`
/// clause. The oracle builds the same unescaped pattern; a caller that
/// needs a literal `%` cannot express one through this function.
///
/// A row with a NULL `summary` never matches: `null like x` is NULL, not
/// true.
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind to match.
/// @param entity_id The entity row id to match.
/// @param pattern The substring to look for in `summary`.
/// @return The matching rows in id order (possibly empty), or
/// `audit_error::query_failed`.
export auto for_entity_grep(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::string_view pattern)
    -> std::expected<std::vector<audit_entry>, audit_error>;

/// @brief A session's header row plus its entries, ordered by `ordinal`.
///
/// @param conn An open, migrated database connection.
/// @param session_id The `sessions` row id.
/// @return The timeline, `audit_error::not_found` when no session has that
/// id, or `audit_error::query_failed`.
export auto session_timeline(db::connection& conn, std::int64_t session_id)
    -> std::expected<session_timeline_result, audit_error>;

} // namespace planar::engine::runtime::audit_trail
