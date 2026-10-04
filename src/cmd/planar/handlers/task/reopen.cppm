/// @file reopen.cppm
/// @brief CLI declaration for `task reopen`.
export module planar.cmd.planar.handlers.task.reopen;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the reopen CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_reopen(CLI::App& task) -> CLI::App* {
  CLI::App* reopen = task.add_subcommand("reopen", "Reopen a done or cancelled task with an audit-trail entry.");
  add_string(*reopen, "--status", k_undocumented);
  add_string(*reopen, "--reason", k_undocumented);
  add_string(*reopen, "--scope", k_undocumented);
  add_bool(*reopen, "--force", "Override active-claim guard and flip status anyway.");
  add_json(*reopen, k_undocumented);
  add_positional(*reopen, "task-id", k_undocumented);
  return reopen;
}
} // namespace planar::cmd::handlers::task_cli
