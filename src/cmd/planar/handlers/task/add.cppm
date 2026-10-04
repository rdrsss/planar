/// @file add.cppm
/// @brief CLI declaration for `task add`.
export module planar.cmd.planar.handlers.task.add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the add CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_add(CLI::App& task) -> CLI::App* {
  CLI::App* add = task.add_subcommand("add", "Create a new task.");
  add_string(*add, "--body", k_undocumented);
  add_string(*add, "--scope", k_undocumented);
  add_string(*add, "--next-action", k_undocumented);
  add_string(*add, "--due", k_undocumented);
  add_int(*add, "--plan", k_undocumented);
  add_int(*add, "--parent", k_undocumented);
  add_string(*add, "--slug", k_undocumented);
  add_int_default(*add, "--priority", "100", k_undocumented);
  add_bool_default_true(*add, "--editor", k_undocumented);
  add_bool(*add, "--no-auto-promote", k_undocumented);
  add_json(*add, k_undocumented);
  add_positional(*add, "title", k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::task_cli
