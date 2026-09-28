/// @file resolve.cppm
/// @brief CLI declaration for `annotate resolve`.
export module planar.cmd.planar.handlers.annotate.resolve;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the resolve CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_resolve(CLI::App& annotate) -> CLI::App* {
  CLI::App* resolve = annotate.add_subcommand("resolve", "Mark an annotation as resolved.");
  add_json(*resolve);
  add_positional(*resolve, "annotation-id");
  return resolve;
}
} // namespace planar::cmd::handlers::annotate_cli
