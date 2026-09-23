/// @file step.cppm
/// @brief CLI declaration for `planar plan step`.
export module planar.cmd.planar.handlers.plan.step;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the step CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_step(CLI::App& plan) -> CLI::App* {
  CLI::App* step = plan.add_subcommand("step", "Manage plan steps.");
  step->require_subcommand(0);
  return step;
}
} // namespace planar::cmd::handlers::plan_cli
