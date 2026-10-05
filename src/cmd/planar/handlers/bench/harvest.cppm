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
  add_int_required(*harvest, "--task", "Task id the harvested touches are attributed to");
  add_string_required(*harvest, "--worktree", "Worktree directory to run git diff in");
  add_string(*harvest, "--base", "Base ref of a committed range; requires --head (default: working tree)");
  add_string(*harvest, "--head", "Head ref of a committed range; requires --base");
  add_positional(*harvest, "run-uid", "Run uid");
  return harvest;
}
} // namespace planar::cmd::handlers::bench_cli
