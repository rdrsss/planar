/// @file routing_build.cppm
/// @brief CLI declaration for `workspace routing_build`.
export module planar.cmd.planar.handlers.workspace.routing_build;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
export auto attach_routing_build(CLI::App* routing) -> CLI::App* {
  CLI::App* build = routing->add_subcommand("build", "Build routing table from workspace membership.");
  add_bool(*build, "--enrich");
  add_json(*build);
  add_positional_optional(*build, "workspace");
  return build;
}
} // namespace planar::cmd::handlers::workspace_cli
