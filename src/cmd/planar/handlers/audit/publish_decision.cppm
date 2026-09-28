/// @file publish_decision.cppm
/// @brief CLI declaration for `audit publish-decision`.
export module planar.cmd.planar.handlers.audit.publish_decision;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::audit_cli {
/// @brief Register the publish decision CLI node.
/// @param audit Input audit.
/// @return Registered CLI node.
export auto attach_publish_decision(CLI::App* audit) -> CLI::App* {
  CLI::App* publish_decision =
      audit->add_subcommand("publish-decision", "Post the decision body to linked operational-plane targets.");
  add_string(*publish_decision, "--scope");
  add_json(*publish_decision);
  add_positional(*publish_decision, "decision-id");
  return publish_decision;
}
} // namespace planar::cmd::handlers::audit_cli
