/// @file file.cppm
/// @brief CLI declaration for `capture file`.
export module planar.cmd.planar.handlers.capture.file;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the file CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_file(CLI::App* capture) -> CLI::App* {
  CLI::App* file = capture->add_subcommand("file", "Attach a file to the active session.");
  add_int(*file, "--session", "Session id (default: the vendor tuple's active session)");
  add_string(*file, "--role", "Role of the file, e.g. implementation target or read for context");
  add_json(*file, "Emit machine-readable JSON instead of text");
  add_positional(*file, "path", "Path of the file that was touched");
  return file;
}
} // namespace planar::cmd::handlers::capture_cli
