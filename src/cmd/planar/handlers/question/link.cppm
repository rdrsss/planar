/// @file link.cppm
/// @brief CLI declaration for `question link`.
export module planar.cmd.planar.handlers.question.link;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::question_cli {
/// @brief Register the link CLI node.
/// @param question Input question.
/// @return Registered CLI node.
export auto attach_link(CLI::App& question) -> CLI::App* {
  CLI::App* link = question.add_subcommand("link", "Create an entity link from a question to another entity.");
  add_string(*link, "--relationship", k_undocumented);
  add_string(*link, "--scope", k_undocumented);
  add_json(*link, k_undocumented);
  add_positional(*link, "question-id", k_undocumented);
  add_positional(*link, "ref", k_undocumented);
  return link;
}
} // namespace planar::cmd::handlers::question_cli
