/// @file link.cppm
/// @brief CLI declaration for `artifact link`.
export module planar.cmd.planar.handlers.artifact.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the link CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_link(CLI::App& artifact) -> CLI::App* {
  CLI::App* link = artifact.add_subcommand("link", "Create an entity link from an artifact to another entity.");
  add_string(*link, "--relationship",
             "Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches");
  add_string(*link, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*link, "Emit machine-readable JSON instead of text");
  add_positional(*link, "artifact-id", "Artifact id");
  add_positional(*link, "ref", "Target entity ref (kind:id)");
  return link;
}
} // namespace planar::cmd::handlers::artifact_cli
