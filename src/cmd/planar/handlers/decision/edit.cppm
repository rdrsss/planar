/// @file edit.cppm
/// @brief CLI declaration for `decision edit`.
export module planar.cmd.planar.handlers.decision.edit;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the edit CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_edit(CLI::App& decision) -> CLI::App* {
  CLI::App* edit = decision.add_subcommand("edit", "Edit a decision in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull", "Accepted for parity; the handler does not read it");
  add_json(*edit, "Accepted for parity; the handler does not read it");
  add_positional(*edit, "decision-id", "Decision id");
  return edit;
}
} // namespace planar::cmd::handlers::decision_cli
