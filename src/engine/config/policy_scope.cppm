/// @file policy_scope.cppm
/// @brief `planar.engine.config.policy_scope` — which association's `policy`
/// table applies to an entity's scope (plan 1142, tech spec component 6).
///
/// `effective_policy` takes an optional association slug. This module derives
/// that slug from the scope a stored entity carries (`scope_kind`, `scope_id`),
/// so `brief`, `claim`, `pull`, `complete` and `spec ingest` resolve the same
/// policy for the same entity:
///
/// - an `association` scope names its association directly;
/// - a `repo` scope (a `projects` row) uses the project's association when it
///   belongs to exactly one;
/// - a project with no membership, a project with several memberships (the
///   choice would be arbitrary) and a `global` scope use the top-level
///   `[policy]` table only, and the reason says which.
///
/// Read-only: one SELECT per call. Error boundary: `std::expected`; nothing
/// throws.
module;

export module planar.engine.config.policy_scope;

import std;
import planar.db;

namespace planar::engine::config {

/// @brief Why `policy_association` returned the slug it did.
export enum class policy_scope_reason : std::uint8_t {
  global_scope,         ///< A global entity: top-level policy only.
  association_scope,    ///< The entity is scoped to the association itself.
  project_membership,   ///< The project belongs to exactly one association.
  project_unassociated, ///< The project belongs to no association: top-level policy only.
  project_ambiguous,    ///< The project belongs to several associations: top-level policy only.
};

/// @brief The wire spelling of a reason, for example `project_membership`.
/// @param reason The reason.
/// @return Its snake_case name.
export auto policy_scope_reason_name(policy_scope_reason reason) -> std::string_view;

/// @brief The association whose policy table applies, with the reason.
export struct policy_association {
  std::optional<std::string> slug; ///< The association slug; unset means top-level policy only.
  policy_scope_reason        reason = policy_scope_reason::global_scope; ///< Why `slug` is what it is.
};

/// @brief Why a resolution failed.
export enum class policy_scope_error : std::uint8_t {
  not_found,    ///< The plan, or the scope row it names, does not exist.
  query_failed, ///< SQL failure.
};

/// @brief Resolve the policy association for a stored `(scope_kind, scope_id)` pair.
/// @param conn An open connection (read-only is sufficient).
/// @param scope_kind The stored kind: `global`, `association` or `repo`.
/// @param scope_id The stored id; ignored for `global`.
/// @return The association and the reason, or `not_found` when the named row is missing or the kind is unknown.
export auto policy_association_for_scope(db::connection& conn, std::string_view scope_kind, std::optional<std::int64_t> scope_id)
    -> std::expected<policy_association, policy_scope_error>;

/// @brief Resolve the policy association for a plan, from the plan's own scope.
/// @param conn An open connection (read-only is sufficient).
/// @param plan_id The plan.
/// @return The association and the reason, or `not_found` when the plan does not exist.
export auto policy_association_for_plan(db::connection& conn, std::int64_t plan_id)
    -> std::expected<policy_association, policy_scope_error>;

} // namespace planar::engine::config
