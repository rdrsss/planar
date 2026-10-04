/// @file import.cppm
/// @brief CLI declaration for `local import`.
export module planar.cmd.planar.handlers.local.importer;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::local_cli {
/// @brief Register the import CLI node.
/// @param local Input local.
/// @return Registered CLI node.
export auto attach_import(CLI::App* local) -> CLI::App* {
  CLI::App* import_leaf = local->add_subcommand("import", "Import a skill or agent from an external directory.");
  add_string(*import_leaf, "--kind", k_undocumented);
  add_bool(*import_leaf, "--force", k_undocumented);
  add_bool(*import_leaf, "--dry-run", k_undocumented);
  add_bool(*import_leaf, "--no-link", k_undocumented);
  add_json(*import_leaf, k_undocumented);
  add_positional(*import_leaf, "path", k_undocumented);
  return import_leaf;
}
} // namespace planar::cmd::handlers::local_cli
