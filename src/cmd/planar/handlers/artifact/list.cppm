/// @file list.cppm
/// @brief CLI declaration for `artifact list`.
export module planar.cmd.planar.handlers.artifact.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the list CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_list(CLI::App& artifact) -> CLI::App* {
  CLI::App* list = artifact.add_subcommand("list", "List artifacts.");
  add_string(*list, "--kind", k_undocumented);
  add_string(*list, "--scope", k_undocumented);
  add_string(*list, "--status", k_undocumented);
  add_int(*list, "--plan", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::artifact_cli
