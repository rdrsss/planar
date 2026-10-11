/// @file add.cppm
/// @brief CLI declaration for `question add`.
export module planar.cmd.planar.handlers.question.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
/// @brief Register the add CLI node.
/// @param question Input question.
/// @return Registered CLI node.
export auto attach_add(CLI::App& question) -> CLI::App* {
  CLI::App* add = question.add_subcommand("add", "Create a new question.");
  add_string(*add, "--body", "Question body text; a leading @ reads it from a file");
  add_string(*add, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_int(*add, "--plan", "Plan id to attach the question to");
  add_bool(*add, "--editor", "Accepted but not implemented; falls back to inline create");
  add_json(*add, "Emit machine-readable JSON instead of text");
  add_positional(*add, "title", "Title of the new question");
  return add;
}
} // namespace planar::cmd::handlers::question_cli
