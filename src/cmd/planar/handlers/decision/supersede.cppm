/// @file supersede.cppm
/// @brief CLI declaration for `decision supersede`.
export module planar.cmd.planar.handlers.decision.supersede;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the supersede CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_supersede(CLI::App& decision) -> CLI::App* {
  CLI::App* supersede = decision.add_subcommand("supersede", "Mark a decision as superseded by a newer decision.");
  add_int_required(*supersede, "--by", "Id of the decision that replaces this one");
  add_string(*supersede, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*supersede, "Emit machine-readable JSON instead of text");
  add_positional(*supersede, "decision-id", "Decision id");
  return supersede;
}
} // namespace planar::cmd::handlers::decision_cli
