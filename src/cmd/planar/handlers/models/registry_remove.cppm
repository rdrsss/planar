/// @file registry_remove.cppm
/// @brief CLI declaration for `models registry remove`.
export module planar.cmd.planar.handlers.models.registry_remove;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
export auto attach_registry_remove(CLI::App& registry) -> CLI::App* {
  CLI::App* remove = registry.add_subcommand("remove", "Remove a candidate when no immutable evidence references it.");
  add_int_required(*remove, "--candidate");
  return remove;
}
} // namespace planar::cmd::handlers::models_cli
