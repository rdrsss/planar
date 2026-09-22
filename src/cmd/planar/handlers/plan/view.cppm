/// @file view.cppm
/// @brief CLI declaration for `planar plan view`.
export module planar.cmd.planar.handlers.plan.view;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
export auto attach_view(CLI::App& plan) -> CLI::App* {
  CLI::App* view = plan.add_subcommand("view", "View a plan's workbench file.");
  add_positional(*view, "plan-id");
  return view;
}
} // namespace planar::cmd::handlers::plan_cli
