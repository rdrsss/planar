/// @file answer.cppm
/// @brief CLI declaration for `question answer`.
export module planar.cmd.planar.handlers.question.answer;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
/// @brief Register the answer CLI node.
/// @param question Input question.
/// @return Registered CLI node.
export auto attach_answer(CLI::App& question) -> CLI::App* {
  CLI::App* answer = question.add_subcommand("answer", "Record an answer to a question.");
  add_string(*answer, "--answer", k_undocumented);
  add_json(*answer, k_undocumented);
  add_positional(*answer, "question-id", k_undocumented);
  return answer;
}
} // namespace planar::cmd::handlers::question_cli
