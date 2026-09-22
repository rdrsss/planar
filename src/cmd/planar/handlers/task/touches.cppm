/// @file touches.cppm
/// @brief CLI declaration for `task touches`.
export module planar.cmd.planar.handlers.task.touches;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_touches(CLI::App& task) -> CLI::App* {
  CLI::App* touches = task.add_subcommand("touches", "Manage repo-touches links on a task.");
  touches->require_subcommand(0);
  return touches;
}
} // namespace planar::cmd::handlers::task_cli
