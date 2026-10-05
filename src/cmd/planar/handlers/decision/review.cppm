/// @file review.cppm
/// @brief CLI declaration for `decision review`.
export module planar.cmd.planar.handlers.decision.review;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::decision_cli {
/// @brief Register the review CLI node.
/// @param decision Input decision.
/// @return Registered CLI node.
export auto attach_review(CLI::App& decision) -> CLI::App* {
  CLI::App* review = decision.add_subcommand("review", "Reviewer entry point for decision diff.");
  add_bool(*review, "--approve", "Report an approve verdict on the pending workbench diff (not persisted)");
  add_bool(*review, "--request-changes",
           "Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve");
  add_json(*review, "Emit machine-readable JSON instead of text");
  add_positional(*review, "decision-id", "Decision id");
  return review;
}
} // namespace planar::cmd::handlers::decision_cli
