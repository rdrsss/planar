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
  add_int(*snapshot, "--session", "Session id to snapshot against (default: the vendor tuple's active session)");
  add_int(*snapshot, "--task", "Task id for the snapshot (default: the session's bound task)");
  add_string(*snapshot, "--note", "Snapshot body; may be @<file>; wins over the body positional");
  add_string(*snapshot, "--next-action", "Next action to record in the snapshot");
  add_json(*snapshot, "Emit machine-readable JSON instead of text");
  add_positional_optional(*snapshot, "body", "Snapshot body; may be @<file>");
  return snapshot;
}
} // namespace planar::cmd::handlers::capture_cli
