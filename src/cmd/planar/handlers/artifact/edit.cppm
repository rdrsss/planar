/// @file edit.cppm
/// @brief CLI declaration for `artifact edit`.
export module planar.cmd.planar.handlers.artifact.edit;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the edit CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_edit(CLI::App& artifact) -> CLI::App* {
  CLI::App* edit = artifact.add_subcommand("edit", "Edit an artifact in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull");
  add_json(*edit);
  add_positional(*edit, "artifact-id");
  return edit;
}
} // namespace planar::cmd::handlers::artifact_cli
