/// @file add.cppm
/// @brief CLI declaration for `question add`.
export module planar.cmd.planar.handlers.question.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
export auto attach_add(CLI::App& question) -> CLI::App* {
  CLI::App* add = question.add_subcommand("add", "Create a new question.");
  add_string(*add, "--body");
  add_string(*add, "--scope");
  add_int(*add, "--plan");
  add_bool(*add, "--editor");
  add_json(*add);
  add_positional(*add, "title");
  return add;
}
} // namespace planar::cmd::handlers::question_cli
