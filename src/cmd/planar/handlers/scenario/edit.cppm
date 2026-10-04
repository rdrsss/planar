/// @file edit.cppm
/// @brief CLI declaration for `scenario edit`.
export module planar.cmd.planar.handlers.scenario.edit;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the edit CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_edit(CLI::App& scenario) -> CLI::App* {
  CLI::App* edit = scenario.add_subcommand("edit", "Edit a scenario in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull", "Accepted for parity; the handler does not read it");
  add_json(*edit, "Accepted for parity; the handler does not read it");
  add_positional(*edit, "scenario-id", "Scenario id");
  return edit;
}
} // namespace planar::cmd::handlers::scenario_cli
