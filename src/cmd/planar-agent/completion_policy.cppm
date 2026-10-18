/// @file completion_policy.cppm
/// @brief `planar.cmd.planar_agent.completion_policy` — binds the configured
/// `review_required` and `task_gate` policy and the build queue's tagged history
/// into `planar-agent complete`.
///
/// The engine decides from values (`planar.engine.runtime.completion`); this
/// module, at layer 3, supplies the three things that engine may not import:
///
///  - the policy, resolved for the completing claim's scope: the claim's task
///    gives its plan, `policy_association_for_plan` gives the association, and
///    `effective_policy` merges association, top level and the embedded default
///    from the `config.toml` the invocation locates (`PLANAR_CONFIG_PATH`, else
///    the default path; no policy value is ever read from the environment);
///  - the gate evidence: `buildqueue::read_tagged_runs` on the database
///    `buildqueue::resolve_queue_db_path` names (the claim token is the tag),
///    turned into `gate_run` rows (`passed` is `outcome = exited` and
///    `exit_code = 0`). The reader opens the file read-only on a second
///    connection and never creates it. A missing file is no evidence; a busy,
///    unreadable or incompatible file, or an environment that names no
///    location, refuses with `task_gate: the build queue database cannot be
///    read: <reason>`;
///  - the refusal text, which names the command that would succeed
///    (`planar-build-queue run --tag <token> -- <gate argv>`).
///
/// The check runs inside `complete`'s transaction through
/// `task_policy::check_completion`. A refusal rolls the transaction back, so
/// the claim stays active and nothing is written. With no policy table in the
/// config file the check reads no database row at all. A config file that
/// cannot be parsed, or whose policy tables `config validate` would refuse,
/// refuses the completion and says so: an unreadable policy must not read as
/// "no policy".
///
/// Error boundary: failures become `agent_error`; nothing throws.
module;

export module planar.cmd.planar_agent.completion_policy;

import std;
import planar.db;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.completion;
import planar.cmd.internal.environment;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

/// @brief What a refused `complete` found, filled by the check and read by the handler that owns the call.
///
/// Handler-owned and live for one verb; the check only writes it. `unmet` is empty when the check did
/// not refuse.
export struct completion_report {
  std::string                                                 claim_token; ///< The completing claim.
  std::vector<engine::runtime::completion::unmet_requirement> unmet;       ///< The unmet requirements, review first.
  std::optional<std::vector<std::string>>                     task_gate;   ///< The gate argv in force, when one is.
  std::string config_problem;                                              ///< Why the policy could not be read; empty otherwise.
};

/// @brief The queued runs a claim's tag names, read from the build queue database.
/// @param env The environment that locates the database (`PLANAR_BUILD_QUEUE_DB`, else `HOME`).
/// @return A lookup that reads the database through `buildqueue::read_tagged_runs`: no runs for a missing
/// file, and the reader's reason (or the resolver's) as the error for any other failure.
export auto history_gate_lookup(const internal::env_lookup& env) -> engine::runtime::completion::gate_run_lookup;

/// @brief The `check_completion` callable `complete` installs on its `task_policy`.
/// @param config_path The located `config.toml`, or unset when none could be located (no policy applies).
/// @param report The slot the check fills on a refusal; must outlive the policy's use.
/// @param env The environment that locates the build queue database; the process environment by default.
/// @return A callable that returns `completion_unmet` to refuse, else success.
export auto completion_check(std::optional<std::filesystem::path> config_path, completion_report* report,
                             internal::env_lookup env = internal::process_env())
    -> std::function<std::expected<void, engine::runtime::agentactivity::agent_error>(
        db::connection&, const engine::runtime::agentactivity::claim&)>;

/// @brief The refusal `complete` prints for a report: one line naming the claim and every unmet
/// requirement, then the command that would succeed, one per line.
/// @param report What the check found.
/// @return The error, tagged `PolicyUnmet`, exit 1.
export auto completion_refusal(const completion_report& report) -> domain_error;

} // namespace planar::cmd::agent
