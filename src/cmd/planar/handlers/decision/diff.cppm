/// @file diff.cppm
/// @brief CLI declaration for `decision diff`.
export module planar.cmd.planar.handlers.decision.diff;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the diff CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_diff(CLI::App& decision) -> CLI::App* {
  CLI::App* diff = decision.add_subcommand("diff", "Diff decision against database version.");
  add_positional(*diff, "decision-id", "Decision id");
  return diff;
}
} // namespace planar::cmd::handlers::decision_cli
