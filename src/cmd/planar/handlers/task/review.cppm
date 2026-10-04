/// @file review.cppm
/// @brief CLI declaration for `task review`.
export module planar.cmd.planar.handlers.task.review;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the review CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_review(CLI::App& task) -> CLI::App* {
  CLI::App* review = task.add_subcommand("review", "Reviewer entry point for task diff.");
  add_bool(*review, "--approve", "Report an approve verdict on the pending workbench diff (not persisted)");
  add_bool(*review, "--request-changes",
           "Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve");
  add_json(*review, "Emit machine-readable JSON instead of text");
  add_positional(*review, "task-id", "Task id");
  return review;
}
} // namespace planar::cmd::handlers::task_cli
