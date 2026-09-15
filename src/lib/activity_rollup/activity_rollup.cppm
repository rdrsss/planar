/// @file activity_rollup.cppm
/// @brief `planar.activity_rollup` — the compact per-entity agent-activity
/// rollup, as a layer-1 primitive.
///
/// ## Why this is layer 1 (decision D19, task 6282)
///
/// The rollup folds a one-line "what has an agent done to this entity"
/// summary onto an arbitrary entity. Two layer-2 buckets want it:
/// `engine_tree` folds it onto every node of `planar tree`, and
/// `engine_runtime` is the natural home for it alongside the two siblings
/// it already carries (`recent_actions_for_entity`,
/// `claim_transitions_for_entity`, used by `audit trail`).
///
/// `engine_tree -> engine_runtime` is an `engine_* -> engine_*` edge, which
/// `cmake/architecture.cmake` FATALs on at configure time (D15/D18). So the
/// rollup previously lived as a deliberate, documented DUPLICATE inside
/// `engine_tree`. That duplication was SAFE only in the narrow sense that
/// `engine_runtime` had not yet ported the rollup itself — there was no
/// second copy to drift from. The moment anyone added one, two
/// implementations of the same three queries would sit in two buckets that
/// cannot see each other, with nothing to detect them diverging.
///
/// Extracting downward is D19's prescribed move — the same one task 6089
/// made for scope resolution and task 6272 made for `planar.process`. Layer
/// 1 is reachable from every layer-2 bucket, so both callers depend on ONE
/// implementation without either depending on the other.
///
/// ## Error boundary
///
/// Every fallible boundary surfaces `std::expected<T, rollup_error>`; no
/// exceptions cross the module boundary. `for_entity` reports a query
/// failure so a caller that cares can distinguish it; `activity_for` folds
/// that to "no activity" for the fold-in callers, which must never fail a
/// whole walk over an additive detail.

module;

export module planar.activity_rollup;

import std;
import planar.db;

export namespace planar::activity_rollup {

/// @brief The compact per-entity agent-activity rollup, or absent when the
/// entity has neither actions nor claims.
///
/// Absent is NOT the same as zeroed: a renderer prints no sub-line and omits
/// the key entirely, so a consumer never sees an empty placeholder.
struct activity_summary {
  std::string  latest_action_kind;     ///< Latest `agent_actions.action_kind`; empty on the claim-only fallback.
  std::string  latest_vendor;          ///< Vendor of the latest action, or of the latest claim on the fallback.
  std::string  last_event_at;          ///< ISO8601 stamp of the latest action or claim transition.
  std::int64_t active_claim_count = 0; ///< Count of active, unexpired claims.
};

/// @brief Why the rollup could not be computed.
enum class rollup_error : std::uint8_t {
  query_failed ///< An underlying SQLite operation failed.
};

/// @brief The per-entity activity rollup.
///
/// Returns absent when the entity has NEITHER actions NOR claims. The claim
/// probe deliberately checks for ANY row, not just an active one: a released
/// claim still represents activity worth surfacing.
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind, e.g. `"task"`.
/// @param entity_id That entity's row id.
/// @return The rollup, `std::nullopt` when there is no activity at all, or
/// the query failure.
[[nodiscard]] auto for_entity(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<std::optional<activity_summary>, rollup_error>;

/// @brief `for_entity` with a query failure folded to "no activity".
///
/// For the fold-in callers: the rollup is an additive detail on a node, and
/// must never fail the whole walk. A caller that needs to tell a failure
/// from genuine absence calls `for_entity` instead.
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind, e.g. `"task"`.
/// @param entity_id That entity's row id.
/// @return The rollup, or `std::nullopt` for both "no activity" and failure.
[[nodiscard]] auto activity_for(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::optional<activity_summary>;

} // namespace planar::activity_rollup
