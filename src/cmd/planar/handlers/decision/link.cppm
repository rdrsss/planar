/// @file link.cppm
/// @brief CLI declaration for `decision link`.
export module planar.cmd.planar.handlers.decision.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
export auto attach_link(CLI::App& decision) -> CLI::App* {
  CLI::App* link = decision.add_subcommand("link", "Create an entity link from a decision to another entity.");
  add_string(*link, "--relationship");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "decision-id");
  add_positional(*link, "ref");
  return link;
}
} // namespace planar::cmd::handlers::decision_cli
