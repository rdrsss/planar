/// @file update.cppm
/// @brief CLI declaration for `task update`.
export module planar.cmd.planar.handlers.task.update;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the update CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_update(CLI::App& task) -> CLI::App* {
  CLI::App* update = task.add_subcommand("update", "Update mutable fields on a task.");
  add_string(*update, "--title", "New title");
  add_string(*update, "--body", "New body; use @<file> to read it from a file");
  add_string(*update, "--status", "New status: todo, doing, blocked, done, cancelled (must be a legal transition)");
  add_string(*update, "--next-action", "New next concrete action");
  add_string(*update, "--due", "New due date (ISO 8601, e.g. 2026-05-15)");
  add_int(*update, "--priority", "New priority integer (lower is higher)");
  add_int(*update, "--plan", "Move the task to this plan id");
  add_string(*update, "--slug", "New slug (globally unique across tasks)");
  add_string(*update, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_bool(*update, "--force", "Bypass the active-claim guard and the status-transition matrix");
  add_string(*update, "--reason", "Rationale recorded on the audit row when --force reopens a terminal task");
  add_bool(*update, "--no-auto-promote", "Skip the plan-status auto-promotion for this update");
  add_bool(*update, "--editor", "Accepted and ignored; use `task edit` for the editor flow");
  add_json(*update, "Emit machine-readable JSON instead of text");
  add_positional(*update, "task-id", "Task id");
  return update;
}
} // namespace planar::cmd::handlers::task_cli
