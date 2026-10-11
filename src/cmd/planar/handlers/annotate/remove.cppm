/// @file remove.cppm
/// @brief CLI declaration for `annotate remove`.
export module planar.cmd.planar.handlers.annotate.remove;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the remove CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_remove(CLI::App& annotate) -> CLI::App* {
  CLI::App* remove = annotate.add_subcommand("remove", "Remove an annotation.");
  add_int(*remove, "--expected-revision", "Annotation revision the caller last saw (required); mismatch refuses");
  add_json(*remove, "Emit machine-readable JSON instead of text");
  add_positional(*remove, "annotation-id", "Annotation id");
  return remove;
}
} // namespace planar::cmd::handlers::annotate_cli
