/// @file registry_update.cppm
/// @brief CLI declaration for `models registry update`.
export module planar.cmd.planar.handlers.models.registry_update;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry update CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_update(CLI::App& registry) -> CLI::App* {
  CLI::App* update = registry.add_subcommand("update", "Update enabled state and deterministic fallback order.");
  add_int_required(*update, "--candidate");
  add_int_required(*update, "--order");
  add_bool(*update, "--disabled");
  return update;
}
} // namespace planar::cmd::handlers::models_cli
