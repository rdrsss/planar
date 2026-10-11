/// @file add.cppm
/// @brief CLI declaration for `assoc add`.
export module planar.cmd.planar.handlers.assoc.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
/// @brief Register the add CLI node.
/// @param assoc Input assoc.
/// @return Registered CLI node.
export auto attach_add(CLI::App* assoc) -> CLI::App* {
  CLI::App* add = assoc->add_subcommand("add", "Add a repo to an association.");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "slug", "Association slug");
  add_positional(*add, "repo-path", "Registered project root path to add to the association");
  return add;
}
} // namespace planar::cmd::handlers::assoc_cli
