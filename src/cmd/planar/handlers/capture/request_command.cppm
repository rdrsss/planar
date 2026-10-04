/// @file request_command.cppm
/// @brief CLI declaration for `capture command`.
export module planar.cmd.planar.handlers.capture.request_command;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the request command CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_request_command(CLI::App* capture) -> CLI::App* {
  CLI::App* command = capture->add_subcommand("command", "Append a command to the active session.");
  add_int(*command, "--session", k_undocumented);
  add_string(*command, "--outcome", k_undocumented);
  add_json(*command, k_undocumented);
  add_positional(*command, "command", k_undocumented);
  return command;
}
} // namespace planar::cmd::handlers::capture_cli
