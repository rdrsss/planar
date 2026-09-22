/// @file withdraw.cppm
/// @brief CLI declaration for `decision withdraw`.
export module planar.cmd.planar.handlers.decision.withdraw;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
export auto attach_withdraw(CLI::App& decision) -> CLI::App* {
  CLI::App* withdraw = decision.add_subcommand("withdraw", "Withdraw a decision.");
  add_string(*withdraw, "--scope");
  add_json(*withdraw);
  add_positional(*withdraw, "decision-id");
  return withdraw;
}
} // namespace planar::cmd::handlers::decision_cli
