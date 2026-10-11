/// @file registry_observe.cppm
/// @brief CLI declaration for `models registry observe`.
export module planar.cmd.planar.handlers.models.registry_observe;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry observe CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_observe(CLI::App& registry) -> CLI::App* {
  CLI::App* observe = registry.add_subcommand("observe", "Append an exact, versioned host capability observation.");
  add_int_required(*observe, "--candidate", "Registry candidate id");
  add_string_required(*observe, "--host", "Host id");
  add_int_required(*observe, "--version", "Observation version");
  add_string_required(*observe, "--availability", "Availability: available, unavailable, unknown");
  add_string_required(*observe, "--spawn-verification", "Spawn verification: verified, unverified, failed, mismatch");
  add_string_required(*observe, "--evidence-ref", "Reference to the evidence for this observation");
  add_string_required(*observe, "--captured-at", "When the observation was captured (timestamp)");
  add_string_required(*observe, "--expires-at", "When the observation expires (timestamp)");
  return observe;
}
} // namespace planar::cmd::handlers::models_cli
