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
  add_int_required(*observe, "--candidate");
  add_string_required(*observe, "--host");
  add_int_required(*observe, "--version");
  add_string_required(*observe, "--availability");
  add_string_required(*observe, "--spawn-verification");
  add_string_required(*observe, "--evidence-ref");
  add_string_required(*observe, "--captured-at");
  add_string_required(*observe, "--expires-at");
  return observe;
}
} // namespace planar::cmd::handlers::models_cli
