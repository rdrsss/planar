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
  add_int(*add, "--after", k_undocumented);
  add_string(*add, "--scope", k_undocumented);
  add_json(*add, k_undocumented);
  add_positional(*add, "plan-id", k_undocumented);
  add_positional(*add, "body", k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::plan_cli
