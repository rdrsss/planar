/// @file list.cppm
/// @brief CLI declaration for `assoc list`.
export module planar.cmd.planar.handlers.assoc.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::assoc_cli {
/// @brief Register the list CLI node.
/// @param assoc Input assoc.
/// @return Registered CLI node.
export auto attach_list(CLI::App* assoc) -> CLI::App* {
  CLI::App* list = assoc->add_subcommand("list", "List all known associations.");
  add_string(*list, "--kind", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::assoc_cli
