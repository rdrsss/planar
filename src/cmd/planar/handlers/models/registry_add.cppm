/// @file registry_add.cppm
/// @brief CLI declaration for `models registry add`.
export module planar.cmd.planar.handlers.models.registry_add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the registry add CLI node.
/// @param registry Input registry.
/// @return Registered CLI node.
export auto attach_registry_add(CLI::App& registry) -> CLI::App* {
  CLI::App* add = registry.add_subcommand("add", "Register one exact opaque candidate identifier.");
  add_string_required(*add, "--vendor", k_undocumented);
  add_string_required(*add, "--id", k_undocumented);
  add_int_required(*add, "--order", k_undocumented);
  add_bool(*add, "--disabled", k_undocumented);
  return add;
}
} // namespace planar::cmd::handlers::models_cli
