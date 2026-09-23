/// @file wontfix.cppm
/// @brief CLI declaration for `question wontfix`.
export module planar.cmd.planar.handlers.question.wontfix;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
/// @brief Register the wontfix CLI node.
/// @param question Input question.
/// @return Registered CLI node.
export auto attach_wontfix(CLI::App& question) -> CLI::App* {
  CLI::App* wontfix = question.add_subcommand("wontfix", "Mark a question as wontfix.");
  add_string(*wontfix, "--reason");
  add_json(*wontfix);
  add_positional(*wontfix, "question-id");
  return wontfix;
}
} // namespace planar::cmd::handlers::question_cli
