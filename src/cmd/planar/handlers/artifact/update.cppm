/// @file update.cppm
/// @brief CLI declaration for `artifact update`.
export module planar.cmd.planar.handlers.artifact.update;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the update CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_update(CLI::App& artifact) -> CLI::App* {
  CLI::App* update = artifact.add_subcommand("update", "Update mutable fields on an artifact.");
  add_string(*update, "--title");
  add_string(*update, "--body");
  add_string(*update, "--source-path");
  add_string(*update, "--status");
  add_string(*update, "--scope");
  add_json(*update);
  add_positional(*update, "artifact-id");
  return update;
}
} // namespace planar::cmd::handlers::artifact_cli
