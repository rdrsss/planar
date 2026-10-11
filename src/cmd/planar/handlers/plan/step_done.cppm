/// @file step_done.cppm
/// @brief CLI declaration for `planar plan step done`.
export module planar.cmd.planar.handlers.plan.step_done;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the step done CLI node.
/// @param step Input step.
/// @return Registered CLI node.
export auto attach_step_done(CLI::App& step) -> CLI::App* {
  CLI::App* done = step.add_subcommand("done", "Mark a plan step as done.");
  add_string(*done, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*done, "Emit machine-readable JSON instead of text");
  add_positional(*done, "step-id", "Plan step id");
  return done;
}
} // namespace planar::cmd::handlers::plan_cli
