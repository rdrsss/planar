/// @file registry_unbind.cppm
/// @brief CLI declaration for `models registry unbind`.
export module planar.cmd.planar.handlers.models.registry_unbind;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry unbind CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_unbind(CLI::App& registry) -> CLI::App* {
  CLI::App* unbind = registry.add_subcommand("unbind", "Remove one explicit role and tier binding.");
  add_int_required(*unbind, "--candidate", "Registry candidate id");
  add_string_required(*unbind, "--role", "Role binding to remove");
  add_string_required(*unbind, "--tier", "Tier: small, medium, large");
  return unbind;
}
} // namespace planar::cmd::handlers::models_cli
