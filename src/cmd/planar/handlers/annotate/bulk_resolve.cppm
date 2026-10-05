/// @file bulk_resolve.cppm
/// @brief CLI declaration for `annotate bulk-resolve`.
export module planar.cmd.planar.handlers.annotate.bulk_resolve;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the bulk resolve CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_bulk_resolve(CLI::App& annotate) -> CLI::App* {
  CLI::App* bulk_resolve = annotate.add_subcommand("bulk-resolve", "Resolve every active annotation matching the filter.");
  add_string(*bulk_resolve, "--operation-id",
             "Operation UUID; runs as a receipt-backed command and records an aggregate receipt");
  add_string(*bulk_resolve, "--anchor-path", "File path the annotation is anchored to");
  add_int(*bulk_resolve, "--plan", "Plan id the annotation is associated with");
  add_int(*bulk_resolve, "--task", "Task id the annotation is associated with");
  add_string(*bulk_resolve, "--vendor", "Vendor that authored the annotation");
  add_string(*bulk_resolve, "--tag", "Tag name");
  add_string(*bulk_resolve, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_json(*bulk_resolve, "Emit machine-readable JSON instead of text");
  return bulk_resolve;
}
} // namespace planar::cmd::handlers::annotate_cli
