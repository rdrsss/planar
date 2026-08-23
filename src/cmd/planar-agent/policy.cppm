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
import planar.engine.runtime.agentatomic;

namespace planar::cmd::agent {

/// @brief The task policy every atomic claim operation in this binary runs
/// under.
///
/// Returns a value rather than a reference to a global, so a test can
/// build its own alongside this one and compare, and so nothing in this
/// binary holds mutable process state.
/// @return The bound policy.
export auto task_policy() -> engine::runtime::agentatomic::task_policy;

} // namespace planar::cmd::agent
