/// @file registry_unbind.cppm
/// @brief CLI declaration for `models registry unbind`.
export module planar.cmd.planar.handlers.models.registry_unbind;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
export auto attach_registry_unbind(CLI::App& registry) -> CLI::App* {
  CLI::App* unbind = registry.add_subcommand("unbind", "Remove one explicit role and tier binding.");
  add_int_required(*unbind, "--candidate");
  add_string_required(*unbind, "--role");
  add_string_required(*unbind, "--tier");
  return unbind;
}
} // namespace planar::cmd::handlers::models_cli
