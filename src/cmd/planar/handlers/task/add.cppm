/// @file add.cppm
/// @brief CLI declaration for `task add`.
export module planar.cmd.planar.handlers.task.add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_add(CLI::App& task) -> CLI::App* {
  CLI::App* add = task.add_subcommand("add", "Create a new task.");
  add_string(*add, "--body");
  add_string(*add, "--scope");
  add_string(*add, "--next-action");
  add_string(*add, "--due");
  add_int(*add, "--plan");
  add_int(*add, "--parent");
  add_string(*add, "--slug");
  add_int_default(*add, "--priority", "100");
  add_bool_default_true(*add, "--editor");
  add_bool(*add, "--no-auto-promote");
  add_json(*add);
  add_positional(*add, "title");
  return add;
}
} // namespace planar::cmd::handlers::task_cli
