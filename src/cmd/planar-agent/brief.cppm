/// @file brief.cppm
/// @brief `planar.cmd.planar_agent.brief` — the composition behind `planar-agent
/// brief <plan>` (plan 1142, tech spec component 9): one read that tells an
/// orchestrator what state a plan is in, who holds which claim, which tasks may
/// run in parallel now, which are ready, which policy applies, and the exact
/// command templates of the claim protocol.
///
/// Layering. The brief joins four engine buckets that may not import one
/// another: `engine/planning` (`recommend`, the parallelizability rules),
/// `engine/runtime` (`next_work`, the claim-aware bucketing `plan next` and
/// `peek` use), `engine/ingest` (the task packet's readiness) and
/// `engine/config` (the policy and the scope-to-association resolver). The
/// composition therefore lives in `cmd_planar_agent`, layer 3, the same place
/// `policy.cpp` binds planning and ingest into the runtime's atomic
/// transactions. It imports engine modules only and nothing from `cmd/planar`.
/// `plan recommend-strategy`'s rule 7 (a task whose packet is not ready is
/// serialized) is re-expressed here from `packet::assemble_task`, because the
/// two engines cannot meet below layer 3; the loop is a few lines.
///
/// Read-only: `gather_brief` only SELECTs and works on a read-only connection.
/// The text rendering stays under 4 KB for a 20-task milestone. The JSON
/// rendering has a fixed key order and carries `brief_version`.
///
/// Extension seam for `confirm_before` rendering (task 7496): `brief::confirm_before`
/// carries the policy's verb paths; the text renderer prints it only when
/// non-empty, and the JSON always carries the array.
///
/// Error boundary: `std::expected<brief, brief_error>`; nothing throws.
module;

export module planar.cmd.planar_agent.brief;

import std;
import planar.db;
import planar.engine.config;

namespace planar::cmd::agent {

/// @brief The version of the brief's JSON shape. Bumped when a key is removed or changes meaning.
export inline constexpr int brief_version = 1;

/// @brief Why `gather_brief` failed.
export enum class brief_error : std::uint8_t {
  plan_not_found, ///< No plan has the id.
  query_failed,   ///< A SQL statement or an engine read failed.
};

/// @brief Quote one word for a POSIX shell: wrap it in single quotes and write an embedded `'` as `'\''`.
///
/// The empty string becomes `''`. Newlines, spaces and `$` stay literal inside the quotes.
/// @param word The word to quote.
/// @return The quoted word.
export auto shell_quote(std::string_view word) -> std::string;

/// @brief Render a duration in seconds as the shortest exact `h`, `m` or `s` form: 3600 is `1h`, 90 is `90s`.
/// @param seconds The duration; zero or less renders `0s`.
/// @return The text.
export auto format_duration(std::int64_t seconds) -> std::string;

/// @brief A plan named in the brief.
export struct brief_plan_ref {
  std::int64_t id = 0; ///< `plans.id`.
  std::string  slug;   ///< `plans.slug`.
  std::string  title;  ///< `plans.title`.
  std::string  status; ///< `plans.status`.
};

/// @brief Task counts by status, in the fixed order the renderers print.
export struct brief_counts {
  std::int64_t todo      = 0; ///< `todo` tasks.
  std::int64_t doing     = 0; ///< `doing` tasks.
  std::int64_t blocked   = 0; ///< `blocked` tasks.
  std::int64_t done      = 0; ///< `done` tasks.
  std::int64_t cancelled = 0; ///< `cancelled` tasks.

  /// @brief Every task counted.
  /// @return The sum.
  [[nodiscard]] auto total() const -> std::int64_t {
    return todo + doing + blocked + done + cancelled;
  }
};

/// @brief One milestone: a child plan of the anchor, with its own counts.
export struct brief_milestone {
  brief_plan_ref plan;   ///< The milestone plan.
  brief_counts   counts; ///< Tasks directly on this milestone.
};

/// @brief A claim that covers work in the plan.
export struct brief_claim {
  std::int64_t               claim_id = 0;  ///< `agent_work_claims.id`.
  std::string                entity;        ///< `task:<id>`, `plan:<id>` or `plan_step:<id>`.
  std::string                state;         ///< `live`, or `stale` for a lapsed lease or a claim marked stale.
  std::optional<std::string> role;          ///< Role the holder named.
  std::optional<std::string> model;         ///< Model the holder named.
  std::optional<std::string> worktree;      ///< Worktree path the holder named.
  std::string                vendor;        ///< Vendor tag.
  std::string                expires_at;    ///< Absolute lease expiry.
  std::vector<std::int64_t>  covered_tasks; ///< Open tasks of this plan the claim covers, ascending.
};

/// @brief One reason a task is held back from the parallel wave.
export struct brief_exclusion {
  int         rule = 0; ///< The strategy rule (1 to 6), 7 for an unready packet, 8 for a covering claim.
  std::string reason;   ///< Operator-facing text without the `excluded by` prefix.
};

/// @brief A task in the wave or in the serialized remainder.
export struct brief_task_ref {
  std::int64_t                 id = 0; ///< `tasks.id`.
  std::optional<std::string>   slug;   ///< `tasks.slug`.
  std::string                  title;  ///< `tasks.title`.
  std::vector<brief_exclusion> why;    ///< Empty for a wave member.
};

/// @brief Packet readiness of one todo task.
export struct brief_readiness {
  std::int64_t               id = 0;  ///< `tasks.id`.
  std::optional<std::string> slug;    ///< `tasks.slug`.
  std::vector<std::string>   reasons; ///< Packet reason names; empty when ready.
};

/// @brief The effective policy for the plan's scope.
export struct brief_policy {
  engine::config::resolved_policy             resolved;    ///< Values and per-key sources.
  std::string                                 digest;      ///< `policy_digest(values)`.
  std::optional<std::string>                  association; ///< The association whose table applied; unset for top level only.
  engine::config::policy_scope_reason         scope_reason = engine::config::policy_scope_reason::global_scope; ///< Why.
  bool                                        valid        = true; ///< False when the config document has policy findings.
  std::vector<engine::config::policy_finding> findings; ///< Findings when invalid; the values then fall through per key.
};

/// @brief One command template of the claim protocol.
export struct brief_template {
  std::string name;    ///< `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `review`, `gate`.
  std::string command; ///< The command line, with placeholders single-quoted.
};

/// @brief The claim protocol as templates.
export struct brief_protocol {
  std::int64_t                lease_ttl_secs       = 0; ///< The default lease TTL a claim gets.
  std::int64_t                heartbeat_every_secs = 0; ///< Half the TTL.
  std::vector<brief_template> templates;                ///< In print order.
};

/// @brief Everything one `brief` call returns.
export struct brief {
  brief_plan_ref                plan;       ///< The plan asked about.
  std::optional<brief_plan_ref> anchor;     ///< The root of the plan's chain; unset when the plan is itself the anchor.
  brief_counts                  counts;     ///< Tasks in the plan and its descendants.
  std::vector<brief_milestone>  milestones; ///< Children of the anchor.
  std::vector<brief_claim>      claims;     ///< Live first, then stale; ascending claim id within each.
  bool                          fan_out_available = false; ///< Whether the wave has at least two tasks.
  std::vector<brief_task_ref>   wave;                      ///< Tasks that may run in parallel now.
  std::vector<brief_task_ref>   serialized;                ///< The remainder of the todo tasks, with reasons.
  std::vector<brief_readiness>  readiness;                 ///< Every unclaimed todo task, ascending by priority then id.
  brief_policy                  policy;                    ///< The effective policy and its source.
  std::vector<std::string>      confirm_before;            ///< The policy's `confirm_before` verb paths; empty unless set.
  brief_protocol                protocol;                  ///< The claim protocol templates.
};

/// @brief Build the protocol templates for a policy.
///
/// Queue templates appear only when `use_queue` is true. With a `task_gate`, the
/// gate template quotes each argv element; without one it names a generic command.
/// @param policy The effective policy values.
/// @param lease_ttl_secs The TTL a claim without `--ttl` gets.
/// @return The protocol.
export auto build_protocol(const engine::config::policy_values& policy, std::int64_t lease_ttl_secs) -> brief_protocol;

/// @brief Compose the brief for a plan. Read-only.
///
/// The policy is resolved with `effective_policy` for the association of the plan's own scope. When the
/// config document has policy findings the brief still renders: `policy.valid` is false, `findings` lists
/// them and the values come from `resolve_policy_lenient`.
/// @param conn An open connection; a read-only one is sufficient.
/// @param plan_id The plan; a milestone or an anchor.
/// @param config_file The user's flattened config file (an empty map for no file).
/// @return The brief, or `plan_not_found` / `query_failed`.
export auto gather_brief(db::connection& conn, std::int64_t plan_id, const engine::config::toml_map& config_file)
    -> std::expected<brief, brief_error>;

/// @brief Render the brief as text, the default output. Ends with a newline.
/// @param value The brief.
/// @return The text.
export auto render_brief_text(const brief& value) -> std::string;

/// @brief Render the brief as JSON with a fixed key order. Ends with a newline.
/// @param value The brief.
/// @return The JSON text, with `brief_version` first.
export auto render_brief_json(const brief& value) -> std::string;

} // namespace planar::cmd::agent
