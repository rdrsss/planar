/// @file event.cppm
/// @brief CLI declaration for `planar bench event`.
export module planar.cmd.planar.handlers.bench.event;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
/// @brief Register the event CLI node.
/// @param bench Input bench.
/// @return Registered CLI node.
export auto attach_event(CLI::App* bench) -> CLI::App* {
  CLI::App* event = bench->add_subcommand("event", "Append a journal event to a run.");
  add_string_required(*event, "--kind", "Journal event kind");
  add_int_required(*event, "--seq", "Event sequence number within the run");
  add_string(*event, "--payload", "Event payload; must be valid JSON");
  add_positional(*event, "run-uid", "Run uid");
  return event;
}
} // namespace planar::cmd::handlers::bench_cli
