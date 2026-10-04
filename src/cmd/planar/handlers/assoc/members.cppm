/// @file members.cppm
/// @brief CLI declaration for `assoc members`.
export module planar.cmd.planar.handlers.assoc.members;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
/// @brief Register the members CLI node.
/// @param assoc Input assoc.
/// @return Registered CLI node.
export auto attach_members(CLI::App* assoc) -> CLI::App* {
  CLI::App* members = assoc->add_subcommand("members", "List all project members of an association.");
  add_json(*members, k_undocumented);
  add_positional(*members, "slug", k_undocumented);
  return members;
}
} // namespace planar::cmd::handlers::assoc_cli
