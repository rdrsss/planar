/// @file list.cppm
/// @brief CLI declaration for `scenario list`.
export module planar.cmd.planar.handlers.scenario.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the list CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_list(CLI::App& scenario) -> CLI::App* {
  CLI::App* list = scenario.add_subcommand("list", "List scenarios.");
  add_string(*list, "--scope", k_undocumented);
  add_string(*list, "--status", k_undocumented);
  add_int(*list, "--related", k_undocumented);
  add_string(*list, "--touches", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::scenario_cli
