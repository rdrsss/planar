/// @file list.cppm
/// @brief CLI declaration for `decision list`.
export module planar.cmd.planar.handlers.decision.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the list CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_list(CLI::App& decision) -> CLI::App* {
  CLI::App* list = decision.add_subcommand("list", "List decisions.");
  add_string(*list, "--scope");
  add_string(*list, "--status");
  add_int(*list, "--plan");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::decision_cli
