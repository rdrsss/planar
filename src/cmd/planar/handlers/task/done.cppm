/// @file done.cppm
/// @brief CLI declaration for `task done`.
export module planar.cmd.planar.handlers.task.done;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the done CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_done(CLI::App& task) -> CLI::App* {
  CLI::App* done = task.add_subcommand("done", "Mark a task as done.");
  add_string(*done, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_bool(*done, "--force", "Override active-claim guard and flip status anyway");
  add_json(*done, "Emit machine-readable JSON instead of text");
  add_positional(*done, "task-id", "Task id");
  return done;
}
} // namespace planar::cmd::handlers::task_cli
