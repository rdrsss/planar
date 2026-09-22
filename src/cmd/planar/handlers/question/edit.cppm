/// @file edit.cppm
/// @brief CLI declaration for `question edit`.
export module planar.cmd.planar.handlers.question.edit;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
export auto attach_edit(CLI::App& question) -> CLI::App* {
  CLI::App* edit = question.add_subcommand("edit", "Edit a question in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull");
  add_json(*edit);
  add_positional(*edit, "question-id");
  return edit;
}
} // namespace planar::cmd::handlers::question_cli
