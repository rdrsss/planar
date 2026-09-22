/// @file touches_list.cppm
/// @brief CLI declaration for `task touches list`.
export module planar.cmd.planar.handlers.task.touches_list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_touches_list(CLI::App& touches) -> CLI::App* {
  CLI::App* list = touches.add_subcommand("list", "List the repo- and path-level touches declared on a task.");
  add_json(*list);
  add_positional(*list, "task-id");
  return list;
}
} // namespace planar::cmd::handlers::task_cli
