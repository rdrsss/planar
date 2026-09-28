/// @file step_add.cppm
/// @brief CLI declaration for `planar plan step add`.
export module planar.cmd.planar.handlers.plan.step_add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the step add CLI node.
/// @param step Input step.
/// @return Registered CLI node.
export auto attach_step_add(CLI::App& step) -> CLI::App* {
  CLI::App* add = step.add_subcommand("add", "Append a new step to a plan.");
  add_int(*add, "--after");
  add_string(*add, "--scope");
  add_json(*add);
  add_positional(*add, "plan-id");
  add_positional(*add, "body");
  return add;
}
} // namespace planar::cmd::handlers::plan_cli
