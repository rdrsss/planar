/// @file tag.cppm
/// @brief CLI declaration for `annotate tag`.
export module planar.cmd.planar.handlers.annotate.tag;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the tag CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_tag(CLI::App& annotate) -> CLI::App* {
  CLI::App* tag = annotate.add_subcommand("tag", "Add or remove a tag on an annotation.");
  add_bool(*tag, "--remove", "Remove the tag instead of adding it");
  add_json(*tag, "Emit machine-readable JSON instead of text");
  add_positional(*tag, "annotation-id", "Annotation id");
  add_positional(*tag, "tag", "Tag to add or remove");
  return tag;
}
} // namespace planar::cmd::handlers::annotate_cli
