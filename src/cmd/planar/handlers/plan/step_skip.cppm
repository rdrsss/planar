/// @file step_skip.cppm
/// @brief CLI declaration for `planar plan step skip`.
export module planar.cmd.planar.handlers.plan.step_skip;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the step skip CLI node.
/// @param step Input step.
/// @return Registered CLI node.
export auto attach_step_skip(CLI::App& step) -> CLI::App* {
  CLI::App* skip = step.add_subcommand("skip", "Mark a plan step as skipped.");
  add_string(*skip, "--scope");
  add_json(*skip);
  add_positional(*skip, "step-id");
  return skip;
}
} // namespace planar::cmd::handlers::plan_cli
