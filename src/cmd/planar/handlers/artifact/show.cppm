/// @file show.cppm
/// @brief CLI declaration for `artifact show`.
export module planar.cmd.planar.handlers.artifact.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the show CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_show(CLI::App& artifact) -> CLI::App* {
  CLI::App* show = artifact.add_subcommand("show", "Show an artifact's metadata and body.");
  add_json(*show, k_undocumented);
  add_positional(*show, "artifact-id", k_undocumented);
  return show;
}
} // namespace planar::cmd::handlers::artifact_cli
