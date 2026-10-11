/// @file bulk_archive.cppm
/// @brief CLI declaration for `annotate bulk-archive`.
export module planar.cmd.planar.handlers.annotate.bulk_archive;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the bulk archive CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_bulk_archive(CLI::App& annotate) -> CLI::App* {
  CLI::App* bulk_archive =
      annotate.add_subcommand("bulk-archive", "Archive every annotation matching the filter (including non-active rows).");
  add_string(*bulk_archive, "--operation-id",
             "Operation UUID; runs as a receipt-backed command and records an aggregate receipt");
  add_string(*bulk_archive, "--anchor-path", "File path the annotation is anchored to");
  add_int(*bulk_archive, "--plan", "Plan id the annotation is associated with");
  add_int(*bulk_archive, "--task", "Task id the annotation is associated with");
  add_string(*bulk_archive, "--vendor", "Vendor that authored the annotation");
  add_string(*bulk_archive, "--tag", "Tag name");
  add_string(*bulk_archive, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_json(*bulk_archive, "Emit machine-readable JSON instead of text");
  return bulk_archive;
}
} // namespace planar::cmd::handlers::annotate_cli
