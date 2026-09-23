/// @file show.cppm
/// @brief CLI declaration for `scenario show`.
export module planar.cmd.planar.handlers.scenario.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the show CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_show(CLI::App& scenario) -> CLI::App* {
  CLI::App* show = scenario.add_subcommand("show", "Show a scenario's details.");
  add_json(*show);
  add_positional(*show, "scenario-id");
  return show;
}
} // namespace planar::cmd::handlers::scenario_cli
