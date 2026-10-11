/// @file end.cppm
/// @brief CLI declaration for `capture end`.
export module planar.cmd.planar.handlers.capture.end;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the end CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_end(CLI::App* capture) -> CLI::App* {
  CLI::App* end = capture->add_subcommand("end", "End the active or specified session.");
  add_int(*end, "--session", "Session id to end; the positional wins when both are given");
  add_string(*end, "--summary", "Session summary text; may be @<file>");
  add_json(*end, "Emit machine-readable JSON instead of text");
  add_positional_optional(*end, "session-id", "Session id to end");
  return end;
}
} // namespace planar::cmd::handlers::capture_cli
