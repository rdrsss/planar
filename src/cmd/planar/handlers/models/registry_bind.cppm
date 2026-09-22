/// @file registry_bind.cppm
/// @brief CLI declaration for `models registry bind`.
export module planar.cmd.planar.handlers.models.registry_bind;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
export auto attach_registry_bind(CLI::App& registry) -> CLI::App* {
  CLI::App* bind = registry.add_subcommand("bind", "Allow one role and tier for a candidate.");
  add_int_required(*bind, "--candidate");
  add_string_required(*bind, "--role");
  add_string_required(*bind, "--tier");
  return bind;
}
} // namespace planar::cmd::handlers::models_cli
