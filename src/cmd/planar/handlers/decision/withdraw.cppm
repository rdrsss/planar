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
  add_string(*withdraw, "--scope", k_undocumented);
  add_json(*withdraw, k_undocumented);
  add_positional(*withdraw, "decision-id", k_undocumented);
  return withdraw;
}
} // namespace planar::cmd::handlers::decision_cli
