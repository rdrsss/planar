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
  add_bool(*review, "--approve");
  add_bool(*review, "--request-changes");
  add_json(*review);
  add_positional(*review, "plan-id");
  return review;
}
} // namespace planar::cmd::handlers::plan_cli
