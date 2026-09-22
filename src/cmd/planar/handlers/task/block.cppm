/// @file block.cppm
/// @brief CLI declaration for `task block`.
export module planar.cmd.planar.handlers.task.block;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_block(CLI::App& task) -> CLI::App* {
  CLI::App* block = task.add_subcommand("block", "Mark a task as blocked and record the blocking relationship.");
  add_int_required(*block, "--on", "Blocking task id");
  add_string(*block, "--reason");
  add_string(*block, "--scope");
  add_bool(*block, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*block);
  add_positional(*block, "task-id");
  return block;
}
} // namespace planar::cmd::handlers::task_cli
