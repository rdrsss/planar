/// @file recommend.cppm
/// @brief CLI declaration for `groups recommend`.
export module planar.cmd.planar.handlers.groups.recommend;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::groups_cli {
/// @brief Register the recommend CLI node.
/// @param groups Input groups.
/// @return Registered CLI node.
export auto attach_recommend(CLI::App* groups) -> CLI::App* {
  CLI::App* recommend = groups->add_subcommand("recommend", "Recommend closure-minimizing task slices for a plan.");
  add_string(*recommend, "--budget", "Per-slice window budget, an unsigned integer (default: 128000)");
  add_string(*recommend, "--solver", "Solver: greedy, mtkahypar (default: greedy)");
  add_json(*recommend, "Emit machine-readable JSON instead of text");
  add_positional(*recommend, "plan-id", "Plan id");
  return recommend;
}
} // namespace planar::cmd::handlers::groups_cli
