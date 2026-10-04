/// @file list.cppm
/// @brief CLI declaration for `links list`.
export module planar.cmd.planar.handlers.links.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::links_cli {
/// @brief Register the list CLI node.
/// @param links Input links.
/// @return Registered CLI node.
export auto attach_list(CLI::App* links) -> CLI::App* {
  CLI::App* list = links->add_subcommand("list", "List entity_links where the given entity is source or target.");
  add_json(*list, k_undocumented);
  add_positional(*list, "ref", k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::links_cli
