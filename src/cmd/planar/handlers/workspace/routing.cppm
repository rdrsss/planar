/// @file routing.cppm
/// @brief CLI declaration for `planar workspace routing`.
export module planar.cmd.planar.handlers.workspace.routing;

import cli11;

namespace planar::cmd::handlers::workspace_cli {
/// @brief Register the routing CLI node.
/// @param workspace Input workspace.
/// @return Registered CLI node.
export auto attach_routing(CLI::App& workspace) -> CLI::App* {
  CLI::App* routing = workspace.add_subcommand("routing", "Manage workspace routing table.");
  routing->require_subcommand(0);
  return routing;
}
} // namespace planar::cmd::handlers::workspace_cli
