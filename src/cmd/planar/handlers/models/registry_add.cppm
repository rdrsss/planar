/// @file registry_add.cppm
/// @brief CLI declaration for `models registry add`.
export module planar.cmd.planar.handlers.models.registry_add;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
export auto attach_registry_add(CLI::App& registry) -> CLI::App* {
  CLI::App* add = registry.add_subcommand("add", "Register one exact opaque candidate identifier.");
  add_string_required(*add, "--vendor");
  add_string_required(*add, "--id");
  add_int_required(*add, "--order");
  add_bool(*add, "--disabled");
  return add;
}
} // namespace planar::cmd::handlers::models_cli
