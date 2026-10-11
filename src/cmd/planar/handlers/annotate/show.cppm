/// @file show.cppm
/// @brief CLI declaration for `annotate show`.
export module planar.cmd.planar.handlers.annotate.show;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the show CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_show(CLI::App& annotate) -> CLI::App* {
  CLI::App* show = annotate.add_subcommand("show", "Show an annotation.");
  add_json(*show, "Emit machine-readable JSON instead of text");
  add_positional(*show, "annotation-id", "Annotation id");
  return show;
}
} // namespace planar::cmd::handlers::annotate_cli
