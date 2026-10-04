/// @file show.cppm
/// @brief CLI declaration for `question show`.
export module planar.cmd.planar.handlers.question.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
/// @brief Register the show CLI node.
/// @param question Input question.
/// @return Registered CLI node.
export auto attach_show(CLI::App& question) -> CLI::App* {
  CLI::App* show = question.add_subcommand("show", "Show a question's details.");
  add_json(*show, k_undocumented);
  add_positional(*show, "question-id", k_undocumented);
  return show;
}
} // namespace planar::cmd::handlers::question_cli
