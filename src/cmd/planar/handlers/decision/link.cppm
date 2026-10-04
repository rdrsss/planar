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
  add_string(*link, "--relationship", k_undocumented);
  add_string(*link, "--scope", k_undocumented);
  add_json(*link, k_undocumented);
  add_positional(*link, "decision-id", k_undocumented);
  add_positional(*link, "ref", k_undocumented);
  return link;
}
} // namespace planar::cmd::handlers::decision_cli
