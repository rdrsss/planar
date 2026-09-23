/// @file harvest.cppm
/// @brief CLI declaration for `planar bench harvest`.
export module planar.cmd.planar.handlers.bench.harvest;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::bench_cli {
/// @brief Register the harvest CLI node.
/// @param bench Input bench.
/// @return Registered CLI node.
export auto attach_harvest(CLI::App* bench) -> CLI::App* {
  CLI::App* harvest = bench->add_subcommand("harvest", "Harvest git diff as actual touches for a run/task.");
  add_int_required(*harvest, "--task");
  add_string_required(*harvest, "--worktree");
  add_string(*harvest, "--base");
  add_string(*harvest, "--head");
  add_positional(*harvest, "run-uid");
  return harvest;
}
} // namespace planar::cmd::handlers::bench_cli
