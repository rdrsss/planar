/// @file diff.cppm
/// @brief CLI declaration for `task diff`.
export module planar.cmd.planar.handlers.task.diff;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_diff(CLI::App& task) -> CLI::App* {
  CLI::App* diff = task.add_subcommand("diff", "Diff task against its database-stored version.");
  add_positional(*diff, "task-id");
  return diff;
}
} // namespace planar::cmd::handlers::task_cli
