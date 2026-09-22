/// @file request_command.cppm
/// @brief CLI declaration for `annotate command`.
export module planar.cmd.planar.handlers.annotate.request_command;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
export auto attach_request_command(CLI::App& annotate) -> CLI::App* {
  CLI::App* command =
      annotate.add_subcommand("command", "Apply a receipt-backed annotation JSON request from stdin (--request @-).");
  add_string(*command, "--request");
  add_json(*command);
  return command;
}
} // namespace planar::cmd::handlers::annotate_cli
