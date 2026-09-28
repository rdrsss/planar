/// @file link.cppm
/// @brief CLI declaration for `scenario link`.
export module planar.cmd.planar.handlers.scenario.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the link CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_link(CLI::App& scenario) -> CLI::App* {
  CLI::App* link = scenario.add_subcommand("link", "Create an entity link from a scenario to another entity.");
  add_string(*link, "--relationship");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "scenario-id");
  add_positional(*link, "ref");
  return link;
}
} // namespace planar::cmd::handlers::scenario_cli
