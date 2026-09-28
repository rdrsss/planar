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
  add_int(*file, "--session");
  add_string(*file, "--role");
  add_json(*file);
  add_positional(*file, "path");
  return file;
}
} // namespace planar::cmd::handlers::capture_cli
