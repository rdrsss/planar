/// @file registry_list.cppm
/// @brief CLI declaration for `models registry list`.
export module planar.cmd.planar.handlers.models.registry_list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry list CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_list(CLI::App& registry) -> CLI::App* {
  CLI::App* list = registry.add_subcommand("list", "List registrations, bindings, and latest observations.");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::models_cli
