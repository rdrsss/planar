/// @file routing.cppm
/// @brief Immutable routing dispatch preview/confirmation transaction.
module;
export module planar.engine.routing;
import std;
import planar.db;

namespace planar::engine::routing {

export enum class error { invalid_value, unknown_preview, stale_preview, query_failed };
export enum class stale_reason {
  expired,
  already_consumed,
  packet_changed,
  profile_changed,
  policy_changed,
  capability_changed,
  cohort_changed,
  claim_changed,
  candidate_changed
};

/// @brief Values persisted by preview and compared byte-for-byte by confirm.
export struct binding {
  std::optional<std::int64_t> task_id;
  std::string  work_item, validation_policy, routing_policy, profile_rule, vendor, role, tier, work_type, complexity;
  std::int64_t project_id = 0, candidate_id = 0;
  std::string  packet_digest, profile_digest, policy_digest, capability_digest, host, assignment_class, evidence_state;
  std::optional<std::int64_t> experiment_id;
  std::optional<std::string>  claim, claim_status;
};
export struct preview_result {
  std::int64_t id;
  std::string  token;
};
export struct confirm_result {
  std::int64_t dispatch_id;
};

export auto valid(std::string_view value) -> bool;
export auto classify_stale(const binding& frozen, const binding& current, std::string_view expires_at, bool consumed,
                           std::string_view now) -> std::optional<stale_reason>;
export auto preview(db::connection& conn, const binding& value, std::string_view expires_at)
    -> std::expected<preview_result, error>;
export auto confirm(db::connection& conn, std::string_view token, std::string_view dispatch_key, const binding& current,
                    std::string_view now, std::string_view reviewer, std::string_view decision, stale_reason* stale = nullptr)
    -> std::expected<confirm_result, error>;
export auto error_name(error value) -> std::string_view;
export auto stale_name(stale_reason value) -> std::string_view;
} // namespace planar::engine::routing
