/// @file remove.cppm
/// @brief CLI declaration for `links remove`.
export module planar.cmd.planar.handlers.links.remove;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::links_cli {
/// @brief Register the remove CLI node.
/// @param links Input links.
/// @return Registered CLI node.
export auto attach_remove(CLI::App* links) -> CLI::App* {
  CLI::App* remove = links->add_subcommand("remove", "Delete an entity_links row by its id.");
  add_json(*remove, "Emit machine-readable JSON instead of text");
  add_positional(*remove, "link-id", "Entity link id");
  return remove;
}
} // namespace planar::cmd::handlers::links_cli
