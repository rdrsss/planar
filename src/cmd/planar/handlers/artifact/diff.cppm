/// @file diff.cppm
/// @brief CLI declaration for `artifact diff`.
export module planar.cmd.planar.handlers.artifact.diff;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the diff CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_diff(CLI::App& artifact) -> CLI::App* {
  CLI::App* diff =
      artifact.add_subcommand("diff", "Show a unified diff between the DB's artifact content and the workbench file.");
  add_positional(*diff, "artifact-id");
  return diff;
}
} // namespace planar::cmd::handlers::artifact_cli
