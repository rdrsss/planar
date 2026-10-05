/// @file event.cppm
/// @brief CLI declaration for `planar run event`.
export module planar.cmd.planar.handlers.run.event;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::run_cli {
/// @brief Register the event CLI node.
/// @param run Input run.
/// @return Registered CLI node.
export auto attach_event(CLI::App* run) -> CLI::App* {
  CLI::App* event = run->add_subcommand("event", "Append a journal event to a run (seq auto-incremented).");
  add_string_required(*event, "--kind", "Event kind, a free-form label (e.g. step, note, error)");
  add_string(*event, "--payload", "Event payload body");
  add_json(*event, "Emit machine-readable JSON instead of text");
  add_positional(*event, "run-uid", "Run uid returned by run start");
  return event;
}
} // namespace planar::cmd::handlers::run_cli
