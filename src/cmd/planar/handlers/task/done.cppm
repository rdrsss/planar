/// @file done.cppm
/// @brief CLI declaration for `task done`.
export module planar.cmd.planar.handlers.task.done;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_done(CLI::App& task) -> CLI::App* {
  CLI::App* done = task.add_subcommand("done", "Mark a task as done (single-arg form; Go supports variadic).");
  add_string(*done, "--scope");
  add_bool(*done, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*done);
  add_positional(*done, "task-id");
  return done;
}
} // namespace planar::cmd::handlers::task_cli
