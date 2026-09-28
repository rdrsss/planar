/// @file add.cppm
/// @brief CLI declaration for `links add`.
export module planar.cmd.planar.handlers.links.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::links_cli {
/// @brief Register the add CLI node.
/// @param links Input links.
/// @return Registered CLI node.
export auto attach_add(CLI::App* links) -> CLI::App* {
  CLI::App* add = links->add_subcommand("add", "Create an entity_links row between two entities.");
  add_string_required(*add, "--relationship");
  add_json(*add);
  add_positional(*add, "from-ref");
  add_positional(*add, "to-ref");
  return add;
}
} // namespace planar::cmd::handlers::links_cli
