/// @file cancel.cppm
/// @brief CLI declaration for `task cancel`.
export module planar.cmd.planar.handlers.task.cancel;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the cancel CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_cancel(CLI::App& task) -> CLI::App* {
  CLI::App* cancel = task.add_subcommand("cancel", "Cancel a task (single-arg form; Go supports variadic).");
  add_string(*cancel, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*cancel, "Emit machine-readable JSON instead of text");
  add_positional(*cancel, "task-id", "Task id");
  return cancel;
}
} // namespace planar::cmd::handlers::task_cli
