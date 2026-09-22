/// @file view.cppm
/// @brief CLI declaration for `scenario view`.
export module planar.cmd.planar.handlers.scenario.view;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
export auto attach_view(CLI::App& scenario) -> CLI::App* {
  CLI::App* view = scenario.add_subcommand("view", "View scenario's workbench file.");
  add_positional(*view, "scenario-id");
  return view;
}
} // namespace planar::cmd::handlers::scenario_cli
