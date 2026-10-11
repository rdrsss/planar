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
  add_string(*list, "--scope", "Restrict to this scope slug instead of the cwd-derived scope");
  add_string(*list, "--status", "Restrict to a status or comma-separated list: open, answered, wontfix");
  add_string(*list, "--touches", "Restrict to entities scoped to or touching this repo slug");
  add_int(*list, "--plan", "Restrict to questions attached to this plan id");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::question_cli
