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
  add_string(*link, "--relationship",
             "Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches");
  add_string(*link, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*link, "Emit machine-readable JSON instead of text");
  add_positional(*link, "scenario-id", "Scenario id");
  add_positional(*link, "ref", "Target entity ref (kind:id)");
  return link;
}
} // namespace planar::cmd::handlers::scenario_cli
