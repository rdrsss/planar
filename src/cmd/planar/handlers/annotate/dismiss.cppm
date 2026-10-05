/// @file dismiss.cppm
/// @brief CLI declaration for `annotate dismiss`.
export module planar.cmd.planar.handlers.annotate.dismiss;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the dismiss CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_dismiss(CLI::App& annotate) -> CLI::App* {
  CLI::App* dismiss = annotate.add_subcommand("dismiss", "Dismiss an annotation.");
  add_json(*dismiss, "Emit machine-readable JSON instead of text");
  add_positional(*dismiss, "annotation-id", "Annotation id");
  return dismiss;
}
} // namespace planar::cmd::handlers::annotate_cli
