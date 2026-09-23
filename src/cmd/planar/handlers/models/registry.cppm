/// @file registry.cppm
/// @brief CLI declaration for `models registry`.
export module planar.cmd.planar.handlers.models.registry;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry CLI node.
/// @param models Input models.
/// @return Registered CLI node.
export auto attach_registry(CLI::App& models) -> CLI::App* {
  CLI::App* registry = models.add_subcommand("registry", "Manage opaque operator candidates and host observations.");
  registry->require_subcommand(0);
  return registry;
}
} // namespace planar::cmd::handlers::models_cli
