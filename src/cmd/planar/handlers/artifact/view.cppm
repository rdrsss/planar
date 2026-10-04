/// @file view.cppm
/// @brief CLI declaration for `artifact view`.
export module planar.cmd.planar.handlers.artifact.view;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the view CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_view(CLI::App& artifact) -> CLI::App* {
  CLI::App* view = artifact.add_subcommand("view", "View the artifact's workbench file in $PAGER.");
  add_positional(*view, "artifact-id", k_undocumented);
  return view;
}
} // namespace planar::cmd::handlers::artifact_cli
