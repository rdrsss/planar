/// @file routing_build.cppm
/// @brief CLI declaration for `workspace routing_build`.
export module planar.cmd.planar.handlers.workspace.routing_build;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
/// @brief Register the routing build CLI node.
/// @param routing Input routing.
/// @return Registered CLI node.
export auto attach_routing_build(CLI::App* routing) -> CLI::App* {
  CLI::App* build = routing->add_subcommand("build", "Build routing table from workspace membership.");
  add_bool(*build, "--enrich", "Merge cached LLM enrichment results into the routing table");
  add_json(*build, "Emit machine-readable JSON instead of text");
  add_positional_optional(*build, "workspace", "Workspace to build the routing table for (default: the cwd-derived workspace)");
  return build;
}
} // namespace planar::cmd::handlers::workspace_cli
