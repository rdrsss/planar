/// @file touches_list.cppm
/// @brief CLI declaration for `task touches list`.
export module planar.cmd.planar.handlers.task.touches_list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the touches list CLI node.
/// @param touches Input touches.
/// @return Registered CLI node.
export auto attach_touches_list(CLI::App& touches) -> CLI::App* {
  CLI::App* list = touches.add_subcommand("list", "List the repo- and path-level touches declared on a task.");
  add_json(*list, k_undocumented);
  add_positional(*list, "task-id", k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::task_cli
