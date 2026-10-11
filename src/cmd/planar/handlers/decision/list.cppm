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
  add_string(*list, "--scope", "Restrict to this scope slug instead of the cwd-derived scope");
  add_string(*list, "--status", "Restrict to a status or comma-separated list: proposed, accepted, withdrawn, superseded");
  add_int(*list, "--plan", "Restrict to decisions attached to this plan id");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::decision_cli
