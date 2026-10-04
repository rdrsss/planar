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
  add_int_required(*observe, "--candidate", k_undocumented);
  add_string_required(*observe, "--host", k_undocumented);
  add_int_required(*observe, "--version", k_undocumented);
  add_string_required(*observe, "--availability", k_undocumented);
  add_string_required(*observe, "--spawn-verification", k_undocumented);
  add_string_required(*observe, "--evidence-ref", k_undocumented);
  add_string_required(*observe, "--captured-at", k_undocumented);
  add_string_required(*observe, "--expires-at", k_undocumented);
  return observe;
}
} // namespace planar::cmd::handlers::models_cli
