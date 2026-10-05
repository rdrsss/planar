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
  add_string(*import_leaf, "--kind", "Target kind: skill, agent (default: skill)");
  add_bool(*import_leaf, "--force", "Overwrite a sandbox file of the same name; otherwise collisions are skipped");
  add_bool(*import_leaf, "--dry-run", "Preview the planned imports and links without writing");
  add_bool(*import_leaf, "--no-link", "Import only; skip the link step");
  add_json(*import_leaf, "Emit machine-readable JSON instead of text");
  add_positional(*import_leaf, "path", "Source .md file, skill directory, or directory of sources");
  return import_leaf;
}
} // namespace planar::cmd::handlers::local_cli
