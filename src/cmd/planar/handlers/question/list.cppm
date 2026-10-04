/// @file list.cppm
/// @brief CLI declaration for `question list`.
export module planar.cmd.planar.handlers.question.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
/// @brief Register the list CLI node.
/// @param question Input question.
/// @return Registered CLI node.
export auto attach_list(CLI::App& question) -> CLI::App* {
  CLI::App* list = question.add_subcommand("list", "List questions.");
  add_string(*list, "--scope", k_undocumented);
  add_string(*list, "--status", k_undocumented);
  add_string(*list, "--touches", k_undocumented);
  add_int(*list, "--plan", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::question_cli
