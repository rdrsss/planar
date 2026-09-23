/// @file view.cppm
/// @brief CLI declaration for `task view`.
export module planar.cmd.planar.handlers.task.view;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the view CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_view(CLI::App& task) -> CLI::App* {
  CLI::App* view = task.add_subcommand("view", "View task's workbench file.");
  add_positional(*view, "task-id");
  return view;
}
} // namespace planar::cmd::handlers::task_cli
