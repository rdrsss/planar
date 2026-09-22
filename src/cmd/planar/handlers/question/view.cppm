/// @file view.cppm
/// @brief CLI declaration for `question view`.
export module planar.cmd.planar.handlers.question.view;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
export auto attach_view(CLI::App& question) -> CLI::App* {
  CLI::App* view = question.add_subcommand("view", "View question's workbench file.");
  add_positional(*view, "question-id");
  return view;
}
} // namespace planar::cmd::handlers::question_cli
