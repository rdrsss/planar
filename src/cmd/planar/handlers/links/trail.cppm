/// @file trail.cppm
/// @brief CLI declaration for `links trail`.
export module planar.cmd.planar.handlers.links.trail;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::links_cli {
/// @brief Register the trail CLI node.
/// @param links Input links.
/// @return Registered CLI node.
export auto attach_trail(CLI::App* links) -> CLI::App* {
  CLI::App* trail = links->add_subcommand("trail", "Show the audit trail for an entity_links row.");
  add_json(*trail, "Emit machine-readable JSON instead of text");
  add_positional(*trail, "link-id", "Entity link id");
  return trail;
}
} // namespace planar::cmd::handlers::links_cli
