/// @file snapshot.cppm
/// @brief CLI declaration for `capture snapshot`.
export module planar.cmd.planar.handlers.capture.snapshot;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the snapshot CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_snapshot(CLI::App* capture) -> CLI::App* {
  CLI::App* snapshot = capture->add_subcommand("snapshot", "Create a context snapshot.");
  add_int(*snapshot, "--session", k_undocumented);
  add_int(*snapshot, "--task", k_undocumented);
  add_string(*snapshot, "--note", k_undocumented);
  add_string(*snapshot, "--next-action", k_undocumented);
  add_json(*snapshot, k_undocumented);
  add_positional_optional(*snapshot, "body", k_undocumented);
  return snapshot;
}
} // namespace planar::cmd::handlers::capture_cli
