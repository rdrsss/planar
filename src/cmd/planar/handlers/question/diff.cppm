/// @file diff.cppm
/// @brief CLI declaration for `question diff`.
export module planar.cmd.planar.handlers.question.diff;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
export auto attach_diff(CLI::App& question) -> CLI::App* {
  CLI::App* diff = question.add_subcommand("diff", "Diff question against database version.");
  add_positional(*diff, "question-id");
  return diff;
}
} // namespace planar::cmd::handlers::question_cli
