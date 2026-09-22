/// @file diff.cppm
/// @brief CLI declaration for `scenario diff`.
export module planar.cmd.planar.handlers.scenario.diff;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
export auto attach_diff(CLI::App& scenario) -> CLI::App* {
  CLI::App* diff = scenario.add_subcommand("diff", "Diff scenario against database version.");
  add_positional(*diff, "scenario-id");
  return diff;
}
} // namespace planar::cmd::handlers::scenario_cli
