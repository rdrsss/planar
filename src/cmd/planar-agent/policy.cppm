/// @file policy.cppm
/// @brief `planar.cmd.planar_agent.policy` — the one place that binds the
/// planning bucket's status matrix and plan roll-up into the agent plane's
/// atomic transactions (plan 996, task 6038).
///
/// `planar.engine.runtime.agentatomic` needs two planning decisions it may
/// not import: the `tasks.status` transition matrix
/// (`planar.engine.planning.transitions`) and the plan-304 roll-up
/// (`planar.engine.planning.plan`). Both are layer 2, and so is
/// `agentatomic` — D15/D18's same-layer prohibition, enforced by
/// `cmake/architecture.cmake` at configure time.
///
/// Layer 3 has no such restriction: `cmd_planar_agent` may depend on as
/// many `engine_*` buckets as it likes. So the seam lives here, in one
/// function, and every handler in this binary uses the same
/// `task_policy` — which is the property worth having, because two
/// handlers with two different notions of "is this transition legal?"
/// would be exactly the kind of divergence nothing would catch.
module;

export module planar.cmd.planar_agent.policy;

import std;
import planar.engine.planning;
import planar.engine.runtime.agentatomic;

namespace planar::cmd::agent {

/// @brief What the in-transaction plan roll-up decided, captured for the handler that owns the call.
///
/// The slot is handler-owned and lives for one verb; the policy only writes it. It holds the
/// `recompute_result` of the last roll-up (`flipped`, `status_after`), so a caller that needs to know
/// whether a milestone was just promoted does not read the plan's status a second time.
export struct plan_roll_up {
  std::optional<engine::planning::recompute_result> result; ///< The last roll-up's outcome; empty when none ran.
};

/// @brief The task policy every atomic claim operation in this binary runs
/// under.
///
/// Returns a value rather than a reference to a global, so a test can
/// build its own alongside this one and compare, and so nothing in this
/// binary holds mutable process state.
/// @param observed When set, receives the plan roll-up's `recompute_result`. It must outlive the
/// policy's use; the roll-up's own failure handling is unchanged.
/// @return The bound policy.
export auto task_policy(plan_roll_up* observed = nullptr) -> engine::runtime::agentatomic::task_policy;

/// @brief Whether a terminal verb just promoted a milestone to `done`: the roll-up flipped the plan
/// to `done` and the call was not an engine replay (a replay wrote nothing, so a retry never reports
/// the milestone twice).
/// @param observed The slot the policy filled.
/// @param result The terminal verb's outcome.
/// @return True only for a fresh promotion.
export auto milestone_promoted(const plan_roll_up& observed, const engine::runtime::agentatomic::terminal_result& result) -> bool;

} // namespace planar::cmd::agent
