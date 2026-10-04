/// @file add.cppm
/// @brief CLI declaration for `scenario add`.
export module planar.cmd.planar.handlers.scenario.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the add CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_add(CLI::App& scenario) -> CLI::App* {
  CLI::App* add = scenario.add_subcommand("add", "Create a new test scenario.");
  add_string(*add, "--body", k_undocumented);
  add_string(*add, "--scope", k_undocumented);
  add_int(*add, "--related", k_undocumented);
  add_int(*add, "--plan", k_undocumented);
  add_bool(*add, "--editor", k_undocumented);
  add_json(*add, k_undocumented);
  add_positional(*add, "title", k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::scenario_cli
