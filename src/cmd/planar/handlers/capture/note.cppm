/// @file note.cppm
/// @brief CLI declaration for `capture note`.
export module planar.cmd.planar.handlers.capture.note;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the note CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_note(CLI::App* capture) -> CLI::App* {
  CLI::App* note = capture->add_subcommand("note", "Append a narrative note to the active session.");
  add_int(*note, "--session", k_undocumented);
  add_json(*note, k_undocumented);
  add_positional(*note, "body", k_undocumented);
  return note;
}
} // namespace planar::cmd::handlers::capture_cli
