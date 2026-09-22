/// @file link.cppm
/// @brief CLI declaration for `question link`.
export module planar.cmd.planar.handlers.question.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
export auto attach_link(CLI::App& question) -> CLI::App* {
  CLI::App* link = question.add_subcommand("link", "Create an entity link from a question to another entity.");
  add_string(*link, "--relationship");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "question-id");
  add_positional(*link, "ref");
  return link;
}
} // namespace planar::cmd::handlers::question_cli
