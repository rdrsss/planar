/// @file completion.cppm
/// @brief `planar.engine.runtime.completion` — what a claim must show before
/// `planar-agent complete` may end it: an independent approving review when the
/// policy requires one, and a passing queued run of the policy's gate argv.
///
/// The module decides from facts only. The policy arrives as a value
/// (`completion_requirements`), so this bucket imports no config module (the
/// `engine_* -> engine_*` edge is forbidden, as for `task_policy`), and the
/// gate evidence arrives through an injected lookup (`gate_run_lookup`) because
/// the build queue's database is another program's, read through the layer-1
/// `planar.buildqueue.reader`, which this bucket does not link. `cmd_planar_agent`
/// binds the lookup to that reader and resolves the policy from `config.toml`;
/// the conformance report calls the same function with a lookup bound the same
/// way.
///
/// **Review.** Satisfied when the claim's LATEST `review` action (by action id)
/// carries the verdict `approve` and was written by a session other than the
/// claim's. A later `request-changes` therefore refuses, naming it; an earlier
/// one that a later approval superseded does not. An action whose verdict is
/// NULL is never a review (`reviews_for_claim` does not return it).
///
/// **Gate.** Satisfied by a run the lookup returns for the claim's token (the queue's tag) that
/// (1) passed, which the lookup reports from `outcome = 'exited'` and
/// `exit_code = 0`, (2) has an argv equal to the policy's argv element by
/// element (no shell splitting, no prefix match, no case folding), and (3) ran
/// in the claim's repository root or its worktree path, each compared after
/// `normalize_directory`: an absolute path with symlinks resolved and no
/// trailing separator. A run in a subdirectory does not count, and a claim that
/// recorded neither path can show no gate run. Either path of the claim
/// suffices, because a claim in a linked worktree records both and a run from
/// the main checkout is not that claim's work. A lookup that fails (the queue
/// database is busy, unreadable or incompatible) is its own unmet requirement,
/// `gate_unreadable`, never "no evidence"; a database that does not exist is a
/// lookup that returns no runs.
///
/// Read-only. Error boundary: `std::expected<..., agent_error>`; nothing throws.
module;

export module planar.engine.runtime.completion;

import std;
import planar.db;
import planar.engine.runtime.agentactivity;

namespace planar::engine::runtime::completion {

/// @brief The completion-time policy for one claim, as plain values.
export struct completion_requirements {
  bool                                    review_required = false; ///< An independent approving review must be recorded.
  std::optional<std::vector<std::string>> task_gate; ///< The argv a passing queued run must match; unset for no gate.

  /// @brief Whether any requirement is set.
  /// @return True when a review or a gate is required.
  [[nodiscard]] auto any() const -> bool {
    return review_required || task_gate.has_value();
  }
};

/// @brief One recorded queued run tagged with the claim's token.
export struct gate_run {
  std::vector<std::string> argv;           ///< The decoded argv the run executed.
  std::string              cwd;            ///< The directory the run executed in, as recorded.
  bool                     passed = false; ///< The run ended `exited` with exit code 0.
};

/// @brief Reads every recorded queued run that carries `tag`.
///
/// Injected by the caller because the build queue's database is not this bucket's. The claim token is the
/// tag. It returns an empty list when the queue database does not exist, may return rows in any order, and
/// need not filter by argv, directory or outcome beyond reporting `passed`. The error string is the reason
/// the database could not be read (busy, unreadable or incompatible) and reaches the operator verbatim.
export using gate_run_lookup = std::function<std::expected<std::vector<gate_run>, std::string>(std::string_view tag)>;

/// @brief Which requirement is unmet.
export enum class requirement_kind : std::uint8_t {
  review_missing,         ///< No review with a verdict is recorded against the claim.
  review_changes,         ///< The latest review is `request-changes`.
  review_not_independent, ///< The latest approving review was written by the claim's own session.
  gate_no_locality,       ///< The claim recorded no repository root or worktree to compare a run against.
  gate_missing,           ///< No passing run with the gate's argv, this claim's token and this claim's directory exists.
  gate_unreadable,        ///< The build queue database exists but could not be read, so the gate cannot be judged.
};

/// @brief One unmet requirement.
export struct unmet_requirement {
  requirement_kind kind{}; ///< Which requirement.
  std::string      text;   ///< One clause, for example `review_required: no review is recorded`; no trailing punctuation.
};

/// @brief Normalize a directory for comparison: absolute, symlinks resolved as far as the path exists,
/// no trailing separator.
/// @param text The recorded path.
/// @return The normalized path, or unset for an empty or relative path (which can match nothing).
export auto normalize_directory(std::string_view text) -> std::optional<std::filesystem::path>;

/// @brief Whether two recorded directories name the same directory after `normalize_directory`.
/// @param lhs One recorded path.
/// @param rhs The other.
/// @return True when both normalize and are equal; false when either is empty or relative.
export auto same_directory(std::string_view lhs, std::string_view rhs) -> bool;

/// @brief The requirements the claim has not met, in the order review then gate.
///
/// Queries only what the policy asks for: with neither key set this reads nothing and returns an empty
/// list. A claim that holds no task (a plan or plan_step claim) has no completion requirements here;
/// `complete` refuses it for being off-task before this matters.
/// @param conn An open connection (read-only is sufficient).
/// @param claim_token The completing claim's token.
/// @param requirements The policy values for the claim's scope.
/// @param lookup Reads the runs tagged with the claim's token; required when `requirements.task_gate` is set.
/// @return The unmet requirements (empty when the claim may complete), `claim_not_found`, or `query_failed`.
export auto unmet_completion_requirements(db::connection& conn, std::string_view claim_token,
                                          const completion_requirements& requirements, const gate_run_lookup& lookup)
    -> std::expected<std::vector<unmet_requirement>, agentactivity::agent_error>;

} // namespace planar::engine::runtime::completion
