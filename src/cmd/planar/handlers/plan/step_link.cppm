/// @file step_link.cppm
/// @brief CLI declaration for `planar plan step link`.
export module planar.cmd.planar.handlers.plan.step_link;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
export auto attach_step_link(CLI::App& step) -> CLI::App* {
  CLI::App* link = step.add_subcommand("link", "Associate a plan step with the task that materializes it.");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "step-id");
  add_positional(*link, "task-id");
  return link;
}
} // namespace planar::cmd::handlers::plan_cli
