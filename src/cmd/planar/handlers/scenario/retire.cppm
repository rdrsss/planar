/// @file retire.cppm
/// @brief CLI declaration for `scenario retire`.
export module planar.cmd.planar.handlers.scenario.retire;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the retire CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_retire(CLI::App& scenario) -> CLI::App* {
  CLI::App* retire = scenario.add_subcommand("retire", "Mark a scenario as retired.");
  add_string(*retire, "--reason");
  add_json(*retire);
  add_positional(*retire, "scenario-id");
  return retire;
}
} // namespace planar::cmd::handlers::scenario_cli
