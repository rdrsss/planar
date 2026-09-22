/// @file list.cppm
/// @brief CLI declaration for `task list`.
export module planar.cmd.planar.handlers.task.list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_list(CLI::App& task) -> CLI::App* {
  CLI::App* list = task.add_subcommand("list", "List tasks.");
  add_string(*list, "--scope");
  add_string(*list, "--status");
  add_int(*list, "--plan");
  add_int(*list, "--priority-max");
  add_string(*list, "--touches");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::task_cli
