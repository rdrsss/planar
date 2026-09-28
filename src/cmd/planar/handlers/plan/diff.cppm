/// @file diff.cppm
/// @brief CLI declaration for `planar plan diff`.
export module planar.cmd.planar.handlers.plan.diff;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the diff CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_diff(CLI::App& plan) -> CLI::App* {
  CLI::App* diff = plan.add_subcommand("diff", "Diff plan against database version.");
  add_positional(*diff, "plan-id");
  return diff;
}
} // namespace planar::cmd::handlers::plan_cli
