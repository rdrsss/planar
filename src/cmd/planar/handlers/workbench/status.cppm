/// @file status.cppm
/// @brief CLI declaration for `workbench status`.
export module planar.cmd.planar.handlers.workbench.status;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the status CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_status(CLI::App& workbench) -> CLI::App* {
  CLI::App* status = workbench.add_subcommand("status", "Show drift and conflicts without writing.");
  add_bool(*status, "--verbose", "Verbose text rendering of the status report");
  add_json(*status, "Emit machine-readable JSON instead of text");
  add_positional_optional(*status, "plan", "Plan id or slug (omit for every plan with a tree)");
  return status;
}
} // namespace planar::cmd::handlers::workbench_cli
