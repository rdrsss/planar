/// @file edit.cppm
/// @brief CLI declaration for `workbench edit`.
export module planar.cmd.planar.handlers.workbench.edit;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the edit CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_edit(CLI::App& workbench) -> CLI::App* {
  CLI::App* edit = workbench.add_subcommand("edit", "Edit a feature's workbench files in $EDITOR.");
  add_string(*edit, "--editor");
  add_json(*edit);
  add_positional(*edit, "plan");
  return edit;
}
} // namespace planar::cmd::handlers::workbench_cli
