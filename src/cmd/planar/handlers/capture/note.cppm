/// @file note.cppm
/// @brief CLI declaration for `capture note`.
export module planar.cmd.planar.handlers.capture.note;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
export auto attach_note(CLI::App* capture) -> CLI::App* {
  CLI::App* note = capture->add_subcommand("note", "Append a narrative note to the active session.");
  add_int(*note, "--session");
  add_json(*note);
  add_positional(*note, "body");
  return note;
}
} // namespace planar::cmd::handlers::capture_cli
