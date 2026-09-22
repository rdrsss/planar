/// @file update.cppm
/// @brief CLI declaration for `task update`.
export module planar.cmd.planar.handlers.task.update;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_update(CLI::App& task) -> CLI::App* {
  CLI::App* update = task.add_subcommand("update", "Update mutable fields on a task.");
  add_string(*update, "--title");
  add_string(*update, "--body");
  add_string(*update, "--status");
  add_string(*update, "--next-action");
  add_string(*update, "--due");
  add_int(*update, "--priority");
  add_int(*update, "--plan");
  add_string(*update, "--slug");
  add_string(*update, "--scope");
  add_bool(*update, "--force");
  add_string(*update, "--reason");
  add_bool(*update, "--no-auto-promote");
  add_bool(*update, "--editor");
  add_json(*update);
  add_positional(*update, "task-id");
  return update;
}
} // namespace planar::cmd::handlers::task_cli
