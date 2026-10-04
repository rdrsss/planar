/// @file diff.cppm
/// @brief CLI declaration for `task diff`.
export module planar.cmd.planar.handlers.task.diff;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the diff CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_diff(CLI::App& task) -> CLI::App* {
  CLI::App* diff = task.add_subcommand("diff", "Diff task against its database-stored version.");
  add_positional(*diff, "task-id", k_undocumented);
  return diff;
}
} // namespace planar::cmd::handlers::task_cli
