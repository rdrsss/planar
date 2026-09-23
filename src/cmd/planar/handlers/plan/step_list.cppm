/// @file step_list.cppm
/// @brief CLI declaration for `planar plan step list`.
export module planar.cmd.planar.handlers.plan.step_list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the step list CLI node.
/// @param step Input step.
/// @return Registered CLI node.
export auto attach_step_list(CLI::App& step) -> CLI::App* {
  CLI::App* list = step.add_subcommand("list", "List steps of a plan.");
  add_json(*list);
  add_positional(*list, "plan-id");
  return list;
}
} // namespace planar::cmd::handlers::plan_cli
