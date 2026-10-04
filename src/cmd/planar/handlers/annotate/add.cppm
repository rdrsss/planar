/// @file add.cppm
/// @brief CLI declaration for `annotate add`.
export module planar.cmd.planar.handlers.annotate.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the add CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_add(CLI::App& annotate) -> CLI::App* {
  CLI::App* add = annotate.add_subcommand("add", "Create a new annotation.");
  add_string(*add, "--anchor-path", "File path the annotation is anchored to (required)");
  add_int(*add, "--line-start", "First line of the anchored range");
  add_int(*add, "--line-end", "Last line of the anchored range");
  add_string(*add, "--commit-sha", "Commit the anchored content was captured at, for later verification");
  add_string(*add, "--text-hash", "Hash of the anchored text, for later drift detection");
  add_string(*add, "--text", "Annotation text");
  add_string(*add, "--title", "Annotation title");
  add_string(*add, "--slug", "Annotation slug");
  add_string(*add, "--body", "Annotation body (literal text)");
  add_string(*add, "--vendor", "Vendor that authored the annotation");
  add_int(*add, "--plan", "Plan id the annotation is associated with");
  add_int(*add, "--task", "Task id the annotation is associated with");
  add_string(*add, "--tags", "Comma-separated tags");
  add_string(*add, "--scope", "Scope slug the annotation is created under");
  add_json(*add, "Emit machine-readable JSON instead of text");
  return add;
}
} // namespace planar::cmd::handlers::annotate_cli
