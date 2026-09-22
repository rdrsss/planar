/// @file snapshot.cppm
/// @brief CLI declaration for `capture snapshot`.
export module planar.cmd.planar.handlers.capture.snapshot;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
export auto attach_snapshot(CLI::App* capture) -> CLI::App* {
  CLI::App* snapshot = capture->add_subcommand("snapshot", "Create a context snapshot.");
  add_int(*snapshot, "--session");
  add_int(*snapshot, "--task");
  add_string(*snapshot, "--note");
  add_string(*snapshot, "--next-action");
  add_json(*snapshot);
  add_positional_optional(*snapshot, "body");
  return snapshot;
}
} // namespace planar::cmd::handlers::capture_cli
