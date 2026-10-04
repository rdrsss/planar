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
  add_string(*add, "--body", k_undocumented);
  add_string(*add, "--rationale", k_undocumented);
  add_int(*add, "--plan", k_undocumented);
  add_string(*add, "--scope", k_undocumented);
  add_bool(*add, "--editor", k_undocumented);
  add_json(*add, k_undocumented);
  add_positional(*add, "title", k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::decision_cli
