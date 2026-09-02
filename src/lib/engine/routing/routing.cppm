/// @file routing.cppm
/// @brief Immutable routing dispatch preview/confirmation transaction.
module;
export module planar.engine.routing;
import std;
import planar.db;

namespace planar::engine::routing {

/// @brief Errors returned by routing preview and confirmation operations.
export enum class error {
  invalid_value,   ///< A required value is empty or contains a control character.
  unknown_preview, ///< No preview has the supplied token.
  stale_preview,   ///< The preview was consumed, expired, or no longer matches its binding.
  query_failed,    ///< An underlying database operation failed.
};

/// @brief Reasons a previously issued preview cannot be confirmed.
export enum class stale_reason {
  expired,            ///< The preview expiry is at or before confirmation time.
  already_consumed,   ///< Another confirmation already consumed the preview.
  packet_changed,     ///< The work packet digest changed.
  profile_changed,    ///< The selected profile digest changed.
  policy_changed,     ///< The routing policy digest changed.
  capability_changed, ///< The capability digest changed.
  cohort_changed,     ///< One of the routing cohort fields changed.
  claim_changed,      ///< Claim identity or status changed.
  candidate_changed,  ///< The requested candidate changed.
};

/// @brief Values persisted by preview and compared byte-for-byte by confirm.
export struct binding {
  std::optional<std::int64_t> task_id;           ///< Bound task, when routing is task-scoped.
  std::string                 work_item;         ///< Logical work-item identifier.
  std::string                 validation_policy; ///< Validation-policy version.
  std::string                 routing_policy;    ///< Routing-policy version.
  std::string                 profile_rule;      ///< Profile-rule version.
  std::string                 vendor;            ///< Selected vendor.
  std::string                 role;              ///< Selected vendor role.
  std::string                 tier;              ///< Routing tier.
  std::string                 work_type;         ///< Classified work type.
  std::string                 complexity;        ///< Classified complexity.
  std::int64_t                project_id   = 0;  ///< Owning project id.
  std::int64_t                candidate_id = 0;  ///< Requested candidate id.
  std::string                 packet_digest;     ///< Work-packet digest.
  std::string                 profile_digest;    ///< Selected-profile digest.
  std::string                 policy_digest;     ///< Routing-policy digest.
  std::string                 capability_digest; ///< Capability-set digest.
  std::string                 host;              ///< Host selected for execution.
  std::string                 assignment_class;  ///< Assignment-class decision.
  std::string                 evidence_state;    ///< Evidence-state decision.
  std::optional<std::int64_t> experiment_id;     ///< Experiment id, when applicable.
  std::optional<std::string>  claim;             ///< Work-claim token, when bound.
  std::optional<std::string>  claim_status;      ///< Claim status captured at preview time.
};
/// @brief The single-use preview token created before dispatch confirmation.
export struct preview_result {
  /// @brief Primary key of the preview row.
  std::int64_t id;
  /// @brief Opaque token the caller must present to `confirm`.
  std::string token;
};
/// @brief The durable dispatch row created by a successful confirmation.
///
/// A confirmation consumes its preview and returns this identifier so the
/// caller can bind subsequent actions to the exact authorization record.
export struct confirm_result {
  /// @brief Primary key of the newly inserted routing dispatch snapshot.
  std::int64_t dispatch_id;
};

/// @brief Test whether a value is non-empty and free of control characters.
/// @param value Candidate wire value.
/// @return True when the value is safe to persist in the routing snapshot.
export auto valid(std::string_view value) -> bool;

/// @brief Explain whether current routing inputs have made a preview stale.
/// @param frozen Binding captured when the preview was created.
/// @param current Binding supplied for confirmation.
/// @param expires_at Preview expiration timestamp.
/// @param consumed Whether the preview was already consumed.
/// @param now Current confirmation timestamp.
/// @return The first stale reason, or unset when confirmation may proceed.
export auto classify_stale(const binding& frozen, const binding& current, std::string_view expires_at, bool consumed,
                           std::string_view now) -> std::optional<stale_reason>;

/// @brief Persist a single-use preview of a validated routing binding.
/// @param conn An open, migrated database connection.
/// @param value Binding to freeze for later confirmation.
/// @param expires_at Preview expiration timestamp.
/// @return Preview id and opaque token, or a routing error.
export auto preview(db::connection& conn, const binding& value, std::string_view expires_at)
    -> std::expected<preview_result, error>;

/// @brief Confirm a preview atomically when its frozen binding is still current.
/// @param conn An open, migrated database connection.
/// @param token Opaque preview token.
/// @param dispatch_key Idempotency key for the durable dispatch row.
/// @param current Binding to compare with the frozen preview.
/// @param now Current confirmation timestamp.
/// @param reviewer Reviewer identity to persist.
/// @param decision Confirmation decision to persist.
/// @param stale Optional destination for a stale-preview reason.
/// @return The durable dispatch id, or a routing error.
export auto confirm(db::connection& conn, std::string_view token, std::string_view dispatch_key, const binding& current,
                    std::string_view now, std::string_view reviewer, std::string_view decision, stale_reason* stale = nullptr)
    -> std::expected<confirm_result, error>;

/// @brief Render a stable public name for a routing error.
/// @param value Error value to render.
/// @return The error's PascalCase public name.
export auto error_name(error value) -> std::string_view;

/// @brief Render a stable wire name for a stale-preview reason.
/// @param value Stale reason to render.
/// @return The reason's snake_case wire name.
export auto stale_name(stale_reason value) -> std::string_view;
} // namespace planar::engine::routing
