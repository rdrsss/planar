/// @file review.cppm
/// @brief CLI declaration for `planar plan review`.
export module planar.cmd.planar.handlers.plan.review;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the review CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_review(CLI::App& plan) -> CLI::App* {
  CLI::App* review = plan.add_subcommand("review", "Reviewer entry point for plan diff.");
  add_bool(*review, "--approve", "Report an approve verdict on the pending workbench diff (not persisted)");
  add_bool(*review, "--request-changes",
           "Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve");
  add_json(*review, "Emit machine-readable JSON instead of text");
  add_positional(*review, "plan-id", "Plan id or slug");
  return review;
}
} // namespace planar::cmd::handlers::plan_cli
