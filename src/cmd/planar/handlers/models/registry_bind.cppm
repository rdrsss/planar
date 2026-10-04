/// @file registry_bind.cppm
/// @brief CLI declaration for `models registry bind`.
export module planar.cmd.planar.handlers.models.registry_bind;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry bind CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_bind(CLI::App& registry) -> CLI::App* {
  CLI::App* bind = registry.add_subcommand("bind", "Allow one role and tier for a candidate.");
  add_int_required(*bind, "--candidate", k_undocumented);
  add_string_required(*bind, "--role", k_undocumented);
  add_string_required(*bind, "--tier", k_undocumented);
  return bind;
}
} // namespace planar::cmd::handlers::models_cli
