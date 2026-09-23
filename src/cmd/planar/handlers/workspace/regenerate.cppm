/// @file regenerate.cppm
/// @brief CLI declaration for `workspace regenerate`.
export module planar.cmd.planar.handlers.workspace.regenerate;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
/// @brief Register the regenerate CLI node.
/// @param workspace Input workspace.
/// @return Registered CLI node.
export auto attach_regenerate(CLI::App* workspace) -> CLI::App* {
  CLI::App* regenerate = workspace->add_subcommand("regenerate", "Regenerate AGENTS.md from current state.");
  add_json(*regenerate);
  add_positional_optional(*regenerate, "workspace");
  return regenerate;
}
} // namespace planar::cmd::handlers::workspace_cli
