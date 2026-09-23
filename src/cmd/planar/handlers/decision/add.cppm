/// @file add.cppm
/// @brief CLI declaration for `decision add`.
export module planar.cmd.planar.handlers.decision.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the add CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_add(CLI::App& decision) -> CLI::App* {
  CLI::App* add = decision.add_subcommand("add", "Create a new decision record.");
  add_string(*add, "--body");
  add_string(*add, "--rationale");
  add_int(*add, "--plan");
  add_string(*add, "--scope");
  add_bool(*add, "--editor");
  add_json(*add);
  add_positional(*add, "title");
  return add;
}
} // namespace planar::cmd::handlers::decision_cli
