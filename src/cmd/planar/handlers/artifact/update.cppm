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
  add_string(*update, "--title", k_undocumented);
  add_string(*update, "--body", k_undocumented);
  add_string(*update, "--source-path", k_undocumented);
  add_string(*update, "--status", k_undocumented);
  add_string(*update, "--scope", k_undocumented);
  add_json(*update, k_undocumented);
  add_positional(*update, "artifact-id", k_undocumented);
  return update;
}
} // namespace planar::cmd::handlers::artifact_cli
