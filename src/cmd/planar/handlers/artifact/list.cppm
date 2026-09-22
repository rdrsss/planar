/// @file list.cppm
/// @brief CLI declaration for `artifact list`.
export module planar.cmd.planar.handlers.artifact.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
export auto attach_list(CLI::App& artifact) -> CLI::App* {
  CLI::App* list = artifact.add_subcommand("list", "List artifacts.");
  add_string(*list, "--kind");
  add_string(*list, "--scope");
  add_string(*list, "--status");
  add_int(*list, "--plan");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::artifact_cli
