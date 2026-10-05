/// @file add.cppm
/// @brief CLI declaration for `task add`.
export module planar.cmd.planar.handlers.task.add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the add CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_add(CLI::App& task) -> CLI::App* {
  CLI::App* add = task.add_subcommand("add", "Create a new task.");
  add_string(*add, "--body", "Task body; use @<file> to read it from a file");
  add_string(*add, "--scope", "Scope slug for the new task instead of the cwd-derived write scope");
  add_string(*add, "--next-action", "Immediate next concrete action (needed for `resume validate` to pass)");
  add_string(*add, "--due", "Due date (ISO 8601, e.g. 2026-05-15)");
  add_int(*add, "--plan", "Plan id to associate the task with");
  add_int(*add, "--parent", "Parent task id, making this a sub-task");
  add_string(*add, "--slug", "Explicit slug (globally unique across tasks)");
  add_int_default(*add, "--priority", "100", "Integer priority; lower is higher");
  add_bool_default_true(*add, "--editor", "Editor body flow; refuses on a TTY with no --body unless --editor=false");
  add_bool(*add, "--no-auto-promote", "Skip the plan-status auto-promotion for this operation");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "title", "Task title");
  return add;
}
} // namespace planar::cmd::handlers::task_cli
