/// @file doctor.cppm
/// @brief CLI declaration for `workspace doctor`.
export module planar.cmd.planar.handlers.workspace.doctor;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
/// @brief Register the doctor CLI node.
/// @param workspace Input workspace.
/// @return Registered CLI node.
export auto attach_doctor(CLI::App* workspace) -> CLI::App* {
  CLI::App* doctor = workspace->add_subcommand("doctor", "Scan and fix workspace registration and state consistency.");
  add_json(*doctor);
  return doctor;
}
} // namespace planar::cmd::handlers::workspace_cli
