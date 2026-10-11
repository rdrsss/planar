/// @file edit.cppm
/// @brief CLI declaration for `task edit`.
export module planar.cmd.planar.handlers.task.edit;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the edit CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_edit(CLI::App& task) -> CLI::App* {
  CLI::App* edit = task.add_subcommand("edit", "Edit a task in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull", "Accepted for parity; the handler does not read it");
  add_json(*edit, "Accepted for parity; the handler does not read it");
  add_positional(*edit, "task-id", "Task id");
  return edit;
}
} // namespace planar::cmd::handlers::task_cli
