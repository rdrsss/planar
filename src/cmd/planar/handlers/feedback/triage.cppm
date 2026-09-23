/// @file triage.cppm
/// @brief CLI declaration for `feedback triage`.
export module planar.cmd.planar.handlers.feedback.triage;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::feedback_cli {
/// @brief Register the triage CLI node.
/// @param feedback Input feedback.
/// @return Registered CLI node.
export auto attach_triage(CLI::App* feedback) -> CLI::App* {
  CLI::App* triage = feedback->add_subcommand("triage", "Review structured feedback triage.");
  triage->require_subcommand(0);
  return triage;
}
} // namespace planar::cmd::handlers::feedback_cli
