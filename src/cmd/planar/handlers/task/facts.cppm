/// @file facts.cppm
/// @brief CLI declaration for `task facts`.
export module planar.cmd.planar.handlers.task.facts;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_facts(CLI::App& task) -> CLI::App* {
  CLI::App* facts = task.add_subcommand("facts", "Manage routing facts on a task.");
  facts->require_subcommand(0);
  return facts;
}
} // namespace planar::cmd::handlers::task_cli
