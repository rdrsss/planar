/// @file list.cppm
/// @brief CLI declaration for `task list`.
export module planar.cmd.planar.handlers.task.list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the list CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_list(CLI::App& task) -> CLI::App* {
  CLI::App* list = task.add_subcommand("list", "List tasks.");
  add_string(*list, "--scope", "Restrict to this scope slug instead of the cwd-derived read set");
  add_string(*list, "--status", "Restrict to a status: todo, doing, blocked, done, cancelled (default: open statuses)");
  add_int(*list, "--plan", "Restrict to tasks of this plan id");
  add_int(*list, "--priority-max", "Only tasks with priority at or below this value");
  add_string(*list, "--touches", "Restrict to tasks scoped to or touching this repo slug");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::task_cli
