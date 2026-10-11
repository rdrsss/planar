/// @file handoff_readiness.cppm
/// @brief CLI declaration for `audit handoff-readiness`.
export module planar.cmd.planar.handlers.audit.handoff_readiness;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::audit_cli {
/// @brief Register the handoff readiness CLI node.
/// @param audit Input audit.
/// @return Registered CLI node.
export auto attach_handoff_readiness(CLI::App* audit) -> CLI::App* {
  CLI::App* handoff_readiness = audit->add_subcommand("handoff-readiness", "Check resume-readiness for all in-flight tasks.");
  add_int_default(*handoff_readiness, "--threshold", "90",
                  "Minimum percent of in-flight tasks that must be resume-ready; exits 1 below it (default: 90)");
  add_json(*handoff_readiness, "Emit machine-readable JSON instead of text");
  return handoff_readiness;
}
} // namespace planar::cmd::handlers::audit_cli
