/// @file reopen.cppm
/// @brief CLI declaration for `task reopen`.
export module planar.cmd.planar.handlers.task.reopen;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_reopen(CLI::App& task) -> CLI::App* {
  CLI::App* reopen = task.add_subcommand("reopen", "Reopen a done or cancelled task with an audit-trail entry.");
  add_string(*reopen, "--status");
  add_string(*reopen, "--reason");
  add_string(*reopen, "--scope");
  add_bool(*reopen, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*reopen);
  add_positional(*reopen, "task-id");
  return reopen;
}
} // namespace planar::cmd::handlers::task_cli
