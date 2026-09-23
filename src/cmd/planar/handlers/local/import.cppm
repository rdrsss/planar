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
  add_string(*import_leaf, "--kind");
  add_bool(*import_leaf, "--force");
  add_bool(*import_leaf, "--dry-run");
  add_bool(*import_leaf, "--no-link");
  add_json(*import_leaf);
  add_positional(*import_leaf, "path");
  return import_leaf;
}
} // namespace planar::cmd::handlers::local_cli
