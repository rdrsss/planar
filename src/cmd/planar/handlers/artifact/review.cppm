/// @file review.cppm
/// @brief CLI declaration for `artifact review`.
export module planar.cmd.planar.handlers.artifact.review;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::artifact_cli {
/// @brief Register the review CLI node.
/// @param artifact Input artifact.
/// @return Registered CLI node.
export auto attach_review(CLI::App& artifact) -> CLI::App* {
  CLI::App* review = artifact.add_subcommand("review", "Reviewer entry point for artifact diff.");
  add_bool(*review, "--approve", "Report an approve verdict on the pending workbench diff (not persisted)");
  add_bool(*review, "--request-changes",
           "Report a request-changes verdict on the pending workbench diff (not persisted); excludes --approve");
  add_json(*review, "Emit machine-readable JSON instead of text");
  add_positional(*review, "artifact-id", "Artifact id");
  return review;
}
} // namespace planar::cmd::handlers::artifact_cli
