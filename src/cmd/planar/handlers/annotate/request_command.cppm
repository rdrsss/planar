/// @file request_command.cppm
/// @brief CLI declaration for `annotate command`.
export module planar.cmd.planar.handlers.annotate.request_command;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the request command CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_request_command(CLI::App& annotate) -> CLI::App* {
  CLI::App* command =
      annotate.add_subcommand("command", "Apply a receipt-backed annotation JSON request from stdin (--request @-).");
  add_string(*command, "--request", "Request source; only @- (JSON object on stdin) is accepted");
  add_json(*command, "Emit machine-readable JSON instead of text");
  return command;
}
} // namespace planar::cmd::handlers::annotate_cli
