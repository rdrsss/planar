/// @file link.cppm
/// @brief CLI declaration for `decision link`.
export module planar.cmd.planar.handlers.decision.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the link CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_link(CLI::App& decision) -> CLI::App* {
  CLI::App* link = decision.add_subcommand("link", "Create an entity link from a decision to another entity.");
  add_string(*link, "--relationship",
             "Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches");
  add_string(*link, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*link, "Emit machine-readable JSON instead of text");
  add_positional(*link, "decision-id", "Decision id");
  add_positional(*link, "ref", "Target entity ref (kind:id)");
  return link;
}
} // namespace planar::cmd::handlers::decision_cli
