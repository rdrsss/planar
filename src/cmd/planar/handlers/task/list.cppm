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
  add_string(*list, "--scope", k_undocumented);
  add_string(*list, "--status", k_undocumented);
  add_int(*list, "--plan", k_undocumented);
  add_int(*list, "--priority-max", k_undocumented);
  add_string(*list, "--touches", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::task_cli
