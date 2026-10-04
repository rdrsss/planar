/// @file show.cppm
/// @brief CLI declaration for `task show`.
export module planar.cmd.planar.handlers.task.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the show CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_show(CLI::App& task) -> CLI::App* {
  CLI::App* show = task.add_subcommand("show", "Show full task details.");
  add_json(*show, k_undocumented);
  add_positional(*show, "task-id", k_undocumented);
  return show;
}
} // namespace planar::cmd::handlers::task_cli
