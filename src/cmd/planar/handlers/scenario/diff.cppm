/// @file diff.cppm
/// @brief CLI declaration for `scenario diff`.
export module planar.cmd.planar.handlers.scenario.diff;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the diff CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_diff(CLI::App& scenario) -> CLI::App* {
  CLI::App* diff = scenario.add_subcommand("diff", "Diff scenario against database version.");
  add_positional(*diff, "scenario-id", k_undocumented);
  return diff;
}
} // namespace planar::cmd::handlers::scenario_cli
