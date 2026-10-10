/// @file policy.cpp
/// @brief Implementation of `planar.cmd.planar_agent.policy`.

module planar.cmd.planar_agent.policy;

import std;
import planar.db;
import planar.engine.planning;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;

namespace planar::cmd::agent {

namespace aa = engine::runtime::agentactivity;

auto task_policy(plan_roll_up* observed) -> engine::runtime::agentatomic::task_policy {
  return engine::runtime::agentatomic::task_policy{
      .check_transition = [](std::string_view from, std::string_view to) -> std::expected<void, aa::agent_error> {
        // `force` is FALSE, always. The agent plane has no `--force` on
        // any status transition: `claim --force` takes over a CLAIM, it
        // does not bypass the task matrix — which is why
        // `claim --entity task:<doing> --force` fails IllegalTransition
        // rather than silently re-entering a task someone else is on.
        auto const checked = engine::planning::check_transition(engine::planning::transition_kind::task, from, to, false);
        if (checked) {
          return {};
        }
        switch (checked.error()) {
        case engine::planning::transition_error::illegal_transition:
          return std::unexpected(aa::agent_error::illegal_transition);
        case engine::planning::transition_error::unknown_status:
          return std::unexpected(aa::agent_error::unknown_status);
        }
        return std::unexpected(aa::agent_error::illegal_transition);
      },
      .recompute_plan = [observed](db::connection& conn, std::int64_t plan_id) -> std::expected<void, aa::agent_error> {
        auto const recomputed = engine::planning::recompute_status(conn, plan_id);
        if (recomputed) {
          if (observed != nullptr) {
            observed->result = *recomputed;
          }
          return {};
        }
        // Every plan_error other than the two transition tags is an
        // internal failure of the roll-up, and the Zig original propagates
        // it out of the terminal transaction — which ROLLS THE WHOLE VERB
        // BACK. That is deliberate and preserved: a claim must not
        // terminalise while leaving the plan's roll-up half-applied.
        switch (recomputed.error()) {
        case engine::planning::plan_error::illegal_transition:
          return std::unexpected(aa::agent_error::illegal_transition);
        case engine::planning::plan_error::unknown_status:
          return std::unexpected(aa::agent_error::unknown_status);
        default:
          return std::unexpected(aa::agent_error::query_failed);
        }
      },
      .clear_unblocked_dependents = [](db::connection& conn, std::int64_t blocker_id) -> std::expected<void, aa::agent_error> {
        auto const cleared = engine::planning::clear_unblocked_dependents(conn, blocker_id);
        if (cleared) {
          return {};
        }
        // The roll-up's only failure mode is a query/statement failure
        // (see the module doc); it does not run the transition matrix, so
        // there is no illegal_transition/unknown_status branch to map.
        return std::unexpected(aa::agent_error::query_failed);
      },
  };
}

auto milestone_promoted(const plan_roll_up& observed, const engine::runtime::agentatomic::terminal_result& result) -> bool {
  return !result.replayed && observed.result.has_value() && observed.result->flipped &&
         observed.result->status_after == engine::planning::plan_status::done;
}

} // namespace planar::cmd::agent
