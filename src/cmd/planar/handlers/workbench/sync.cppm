/// @file sync.cppm
/// @brief CLI declaration for `workbench sync`.
export module planar.cmd.planar.handlers.workbench.sync;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the sync CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_sync(CLI::App& workbench) -> CLI::App* {
  CLI::App* sync = workbench.add_subcommand("sync", "Atomically apply FS and DB changes via a unified sync.");
  add_bool(*sync, "--verbose", "Verbose text rendering of the sync result");
  add_json(*sync, "Emit machine-readable JSON instead of text");
  add_positional(*sync, "plan", "Plan id or slug");
  return sync;
}
} // namespace planar::cmd::handlers::workbench_cli
