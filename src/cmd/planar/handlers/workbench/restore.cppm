/// @file restore.cppm
/// @brief CLI declaration for `workbench restore`.
export module planar.cmd.planar.handlers.workbench.restore;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the restore CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_restore(CLI::App& workbench) -> CLI::App* {
  CLI::App* restore = workbench.add_subcommand("restore", "Restore an archived feature's workbench tree.");
  add_json(*restore, "Emit machine-readable JSON instead of text");
  add_string(*restore, "--filter-mode", "Terminal-status filter: 'failures' (default) or 'all'");
  add_positional(*restore, "plan", "Plan id or slug");
  return restore;
}
} // namespace planar::cmd::handlers::workbench_cli
