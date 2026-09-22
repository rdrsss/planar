/// @file show.cppm
/// @brief CLI declaration for `task show`.
export module planar.cmd.planar.handlers.task.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_show(CLI::App& task) -> CLI::App* {
  CLI::App* show = task.add_subcommand("show", "Show full task details.");
  add_json(*show);
  add_positional(*show, "task-id");
  return show;
}
} // namespace planar::cmd::handlers::task_cli
