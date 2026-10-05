/// @file bulk_dismiss.cppm
/// @brief CLI declaration for `annotate bulk-dismiss`.
export module planar.cmd.planar.handlers.annotate.bulk_dismiss;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the bulk dismiss CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_bulk_dismiss(CLI::App& annotate) -> CLI::App* {
  CLI::App* bulk_dismiss = annotate.add_subcommand("bulk-dismiss", "Dismiss every active annotation matching the filter.");
  add_string(*bulk_dismiss, "--operation-id",
             "Operation UUID; runs as a receipt-backed command and records an aggregate receipt");
  add_string(*bulk_dismiss, "--anchor-path", "File path the annotation is anchored to");
  add_int(*bulk_dismiss, "--plan", "Plan id the annotation is associated with");
  add_int(*bulk_dismiss, "--task", "Task id the annotation is associated with");
  add_string(*bulk_dismiss, "--vendor", "Vendor that authored the annotation");
  add_string(*bulk_dismiss, "--tag", "Tag name");
  add_string(*bulk_dismiss, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_json(*bulk_dismiss, "Emit machine-readable JSON instead of text");
  return bulk_dismiss;
}
} // namespace planar::cmd::handlers::annotate_cli
