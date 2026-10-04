/// @file update.cppm
/// @brief CLI declaration for `task update`.
export module planar.cmd.planar.handlers.task.update;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the update CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_update(CLI::App& task) -> CLI::App* {
  CLI::App* update = task.add_subcommand("update", "Update mutable fields on a task.");
  add_string(*update, "--title", k_undocumented);
  add_string(*update, "--body", k_undocumented);
  add_string(*update, "--status", k_undocumented);
  add_string(*update, "--next-action", k_undocumented);
  add_string(*update, "--due", k_undocumented);
  add_int(*update, "--priority", k_undocumented);
  add_int(*update, "--plan", k_undocumented);
  add_string(*update, "--slug", k_undocumented);
  add_string(*update, "--scope", k_undocumented);
  add_bool(*update, "--force", k_undocumented);
  add_string(*update, "--reason", k_undocumented);
  add_bool(*update, "--no-auto-promote", k_undocumented);
  add_bool(*update, "--editor", k_undocumented);
  add_json(*update, k_undocumented);
  add_positional(*update, "task-id", k_undocumented);
  return update;
}
} // namespace planar::cmd::handlers::task_cli
