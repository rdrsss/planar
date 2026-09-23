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
  add_bool(*status, "--verbose");
  add_json(*status);
  add_positional_optional(*status, "plan");
  return status;
}
} // namespace planar::cmd::handlers::workbench_cli
