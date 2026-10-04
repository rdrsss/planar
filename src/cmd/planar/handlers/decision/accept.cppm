/// @file accept.cppm
/// @brief CLI declaration for `decision accept`.
export module planar.cmd.planar.handlers.decision.accept;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the accept CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_accept(CLI::App& decision) -> CLI::App* {
  CLI::App* accept = decision.add_subcommand("accept", "Accept a proposed decision.");
  add_string(*accept, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_json(*accept, "Emit machine-readable JSON instead of text");
  add_positional(*accept, "decision-id", "Decision id");
  return accept;
}
} // namespace planar::cmd::handlers::decision_cli
