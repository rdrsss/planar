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
  add_bool(*edit, "--no-pull", k_undocumented);
  add_json(*edit, k_undocumented);
  add_positional(*edit, "scenario-id", k_undocumented);
  return edit;
}
} // namespace planar::cmd::handlers::scenario_cli
