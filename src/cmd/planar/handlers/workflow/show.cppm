/// @file show.cppm
/// @brief CLI declaration for `workflow show`.
export module planar.cmd.planar.handlers.workflow.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workflow_cli {
/// @brief Register the show CLI node.
/// @param workflow Input workflow.
/// @return Registered CLI node.
export auto attach_show(CLI::App* workflow) -> CLI::App* {
  CLI::App* show = workflow->add_subcommand("show", "Show @meta and source path for a named workflow.");
  add_json(*show);
  add_positional(*show, "name");
  return show;
}
} // namespace planar::cmd::handlers::workflow_cli
