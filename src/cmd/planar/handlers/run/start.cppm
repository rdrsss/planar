/// @file start.cppm
/// @brief CLI declaration for `planar run start`.
export module planar.cmd.planar.handlers.run.start;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::run_cli {
/// @brief Register the start CLI node.
/// @param run Input run.
/// @return Registered CLI node.
export auto attach_start(CLI::App* run) -> CLI::App* {
  CLI::App* start = run->add_subcommand("start", "Mint a new operational run record and print its run_uid as JSON.");
  add_int_required(*start, "--plan");
  add_string(*start, "--workflow");
  add_json(*start);
  return start;
}
} // namespace planar::cmd::handlers::run_cli
