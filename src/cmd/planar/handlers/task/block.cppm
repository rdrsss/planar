/// @file block.cppm
/// @brief CLI declaration for `task block`.
export module planar.cmd.planar.handlers.task.block;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the block CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_block(CLI::App& task) -> CLI::App* {
  CLI::App* block = task.add_subcommand("block", "Mark a task as blocked and record the blocking relationship.");
  add_int_required(*block, "--on", "Blocking task id");
  add_string(*block, "--reason", "Optional reason for the block");
  add_string(*block, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_bool(*block, "--force", "Override active-claim guard and flip status anyway");
  add_json(*block, "Emit machine-readable JSON instead of text");
  add_positional(*block, "task-id", "Task id");
  return block;
}
} // namespace planar::cmd::handlers::task_cli
