/// @file registry_verify_identity.cppm
/// @brief CLI declaration for `models registry verify identity`.
export module planar.cmd.planar.handlers.models.registry_verify_identity;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry verify identity CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_verify_identity(CLI::App& registry) -> CLI::App* {
  CLI::App* verify_identity =
      registry.add_subcommand("verify-identity", "Compare requested and actual spawn identity without aliasing.");
  add_int_required(*verify_identity, "--candidate", "Registry candidate id");
  add_string_required(*verify_identity, "--actual-vendor", "Vendor the spawn actually reported");
  add_string_required(*verify_identity, "--actual-id", "Candidate identifier the spawn actually reported");
  return verify_identity;
}
} // namespace planar::cmd::handlers::models_cli
