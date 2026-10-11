/// @file remove.cppm
/// @brief CLI declaration for `assoc remove`.
export module planar.cmd.planar.handlers.assoc.remove;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
/// @brief Register the remove CLI node.
/// @param assoc Input assoc.
/// @return Registered CLI node.
export auto attach_remove(CLI::App* assoc) -> CLI::App* {
  CLI::App* remove = assoc->add_subcommand("remove", "Remove a repo from an association.");
  add_json(*remove, "Emit machine-readable JSON instead of text");
  add_positional(*remove, "slug", "Association slug");
  add_positional(*remove, "repo-path", "Project root path to remove from the association");
  return remove;
}
} // namespace planar::cmd::handlers::assoc_cli
