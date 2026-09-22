/// @file link.cppm
/// @brief CLI declaration for `artifact link`.
export module planar.cmd.planar.handlers.artifact.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
export auto attach_link(CLI::App& artifact) -> CLI::App* {
  CLI::App* link = artifact.add_subcommand("link", "Create an entity link from an artifact to another entity.");
  add_string(*link, "--relationship");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "artifact-id");
  add_positional(*link, "ref");
  return link;
}
} // namespace planar::cmd::handlers::artifact_cli
