/// @file routing_show.cppm
/// @brief CLI declaration for `workspace routing_show`.
export module planar.cmd.planar.handlers.workspace.routing_show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
export auto attach_routing_show(CLI::App* routing) -> CLI::App* {
  CLI::App* show = routing->add_subcommand("show", "Display current routing table.");
  add_json(*show);
  add_positional_optional(*show, "workspace");
  return show;
}
} // namespace planar::cmd::handlers::workspace_cli
