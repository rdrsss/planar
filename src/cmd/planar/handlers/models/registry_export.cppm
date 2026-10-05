/// @file registry_export.cppm
/// @brief CLI declaration for `models registry export`.
export module planar.cmd.planar.handlers.models.registry_export;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry export CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_export(CLI::App& registry) -> CLI::App* {
  CLI::App* export_cmd = registry.add_subcommand("export", "Export the versioned registry compatibility document.");
  add_json(*export_cmd, "Emit machine-readable JSON instead of text");
  return export_cmd;
}
} // namespace planar::cmd::handlers::models_cli
