/// @file archive.cppm
/// @brief CLI declaration for `workbench archive`.
export module planar.cmd.planar.handlers.workbench.archive;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the archive CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_archive(CLI::App& workbench) -> CLI::App* {
  CLI::App* archive = workbench.add_subcommand("archive", "Archive a feature's workbench filesystem tree.");
  add_json(*archive, k_undocumented);
  add_string(*archive, "--filter-mode", "Terminal-status filter: 'failures' (default) or 'all'");
  add_positional(*archive, "plan", k_undocumented);
  return archive;
}
} // namespace planar::cmd::handlers::workbench_cli
