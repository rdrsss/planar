/// @file update.cppm
/// @brief CLI declaration for `annotate update`.
export module planar.cmd.planar.handlers.annotate.update;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the update CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_update(CLI::App& annotate) -> CLI::App* {
  CLI::App* update = annotate.add_subcommand("update", "Update an annotation.");
  add_string(*update, "--title", "Annotation title");
  add_string(*update, "--slug", "Annotation slug");
  add_string(*update, "--body", "Annotation body (literal text)");
  add_string(*update, "--status", "New status: active, resolved, dismissed, archived");
  add_int(*update, "--plan", "Plan id the annotation is associated with");
  add_int(*update, "--task", "Task id the annotation is associated with");
  add_string(*update, "--scope", "Move the annotation to this scope slug (patch field)");
  add_json(*update, "Emit machine-readable JSON instead of text");
  add_positional(*update, "annotation-id", "Annotation id");
  return update;
}
} // namespace planar::cmd::handlers::annotate_cli
