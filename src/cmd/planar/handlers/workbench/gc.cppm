/// @file gc.cppm
/// @brief CLI declaration for `workbench gc`.
export module planar.cmd.planar.handlers.workbench.gc;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the gc CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_gc(CLI::App& workbench) -> CLI::App* {
  CLI::App* gc = workbench.add_subcommand("gc", "Remove FS files whose backing entity is terminal in the DB.");
  add_bool(*gc, "--dry-run", "Preview only; do not touch disk");
  add_bool(*gc, "--yes", "Discard FS-content drift; remove drifted files anyway");
  add_string(*gc, "--filter-mode", "Terminal-status filter: 'failures' (default) or 'all'");
  add_bool(*gc, "--all-scopes", "Walk every plan's workbench tree");
  add_json(*gc, k_undocumented);
  add_positional_optional(*gc, "plan", k_undocumented);
  return gc;
}
} // namespace planar::cmd::handlers::workbench_cli
