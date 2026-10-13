/// @file claim_policy.cppm
/// @brief `planar.cmd.planar_agent.claim_policy` — binds the configured policy into
/// `planar-agent claim` and `pull` (plan 1142, tech spec component 6).
///
/// The runtime's claim transaction asks one question of this module, through
/// `task_policy::claim_policy`: may this claim be taken, and under which policy? The answer comes from
/// the `config.toml` the invocation locates (`PLANAR_CONFIG_PATH`, else the default path; no policy value
/// is ever read from the environment), resolved for the claim's scope:
///
///  - a task claim: the task's plan decides the association (`policy_association_for_plan`); a task with
///    no plan falls back to its own scope;
///  - a plan claim, and `pull <plan-id>`: the plan itself;
///  - a plan_step claim: the step's plan, else top-level policy only.
///
/// What the answer carries:
///
///  - the digest of the effective policy (`policy_digest`), recorded on every claim of every role and
///    entity kind as `agent_work_claims.policy_digest`;
///  - a refusal when `require_model` is set and a `coder` claim names no model, or names an empty one.
///    Roles other than `coder` are not affected, and a `pull` without `--role` is not a `coder` claim.
///
/// A config file that cannot be read or parsed, or whose policy tables `config validate` would refuse,
/// has no policy to apply: the claim records no digest (NULL). It proceeds unless its role is `coder`,
/// which could be bound by a `require_model` the broken file may carry, so a coder claim is refused and
/// the refusal says so. A broken file therefore never blocks unrelated work.
///
/// The check runs inside the claim's transaction; a refusal rolls it back and writes nothing. Error
/// boundary: failures become `agent_error`; nothing throws.
module;

export module planar.cmd.planar_agent.claim_policy;

import std;
import planar.db;
import planar.engine.config;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

/// @brief The association whose `policy` table applies to a claimed entity.
/// @param conn An open connection (read-only is sufficient).
/// @param kind What is claimed.
/// @param entity_id Which one.
/// @return The association slug (unset for top-level policy only), or `query_failed`.
export auto policy_association_for_entity(db::connection& conn, engine::runtime::agentactivity::entity_kind kind,
                                          std::int64_t entity_id)
    -> std::expected<std::optional<std::string>, engine::runtime::agentactivity::agent_error>;

/// @brief What a refused `claim` or `pull` found, filled by the decision and read by the handler that owns the call.
///
/// Handler-owned and live for one verb; the decision only writes it. `text` is empty when nothing was refused.
export struct claim_policy_report {
  std::string text; ///< The refusal's body, without the `error: ` prefix.
};

/// @brief The `claim_policy` callable `claim` and `pull` install on their `task_policy`.
/// @param config_path The located `config.toml`, or unset when none could be located (default policy).
/// @param verb `claim` or `pull`, named in a refusal.
/// @param report The slot the decision fills on a refusal; must outlive the policy's use.
/// @return A callable that returns the policy digest, or `claim_policy_unmet` to refuse.
export auto claim_policy_decision(std::optional<std::filesystem::path> config_path, std::string verb, claim_policy_report* report)
    -> std::function<std::expected<std::optional<std::string>, engine::runtime::agentactivity::agent_error>(
        db::connection&, const engine::runtime::agentatomic::claim_request&)>;

/// @brief The refusal `claim` and `pull` print for a report.
/// @param report What the decision found.
/// @return The error, tagged `PolicyUnmet`, exit 1.
export auto claim_policy_refusal(const claim_policy_report& report) -> domain_error;

} // namespace planar::cmd::agent
