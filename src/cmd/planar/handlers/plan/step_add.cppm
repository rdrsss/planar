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
  add_int(*add, "--after", "Insert after this step ordinal, renumbering later steps (default: append)");
  add_string(*add, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "plan-id", "Plan id or slug");
  add_positional(*add, "body", "Step body text");
  return add;
}
} // namespace planar::cmd::handlers::plan_cli
