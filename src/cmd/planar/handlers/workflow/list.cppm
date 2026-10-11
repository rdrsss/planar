/// @file list.cppm
/// @brief CLI declaration for `workflow list`.
export module planar.cmd.planar.handlers.workflow.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workflow_cli {
/// @brief Register the list CLI node.
/// @param workflow Input workflow.
/// @return Registered CLI node.
export auto attach_list(CLI::App* workflow) -> CLI::App* {
  CLI::App* list = workflow->add_subcommand("list", "List shipped and sandbox workflows.");
  add_bool(*list, "--local", "Show only sandbox (local) workflows");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::workflow_cli
