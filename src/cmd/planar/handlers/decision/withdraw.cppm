/// @file withdraw.cppm
/// @brief CLI declaration for `decision withdraw`.
export module planar.cmd.planar.handlers.decision.withdraw;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the withdraw CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_withdraw(CLI::App& decision) -> CLI::App* {
  CLI::App* withdraw = decision.add_subcommand("withdraw", "Withdraw a decision.");
  add_string(*withdraw, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_json(*withdraw, "Emit machine-readable JSON instead of text");
  add_positional(*withdraw, "decision-id", "Decision id");
  return withdraw;
}
} // namespace planar::cmd::handlers::decision_cli
