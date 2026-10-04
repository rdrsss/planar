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
  add_string(*bulk_archive, "--operation-id", k_undocumented);
  add_string(*bulk_archive, "--anchor-path", k_undocumented);
  add_int(*bulk_archive, "--plan", k_undocumented);
  add_int(*bulk_archive, "--task", k_undocumented);
  add_string(*bulk_archive, "--vendor", k_undocumented);
  add_string(*bulk_archive, "--tag", k_undocumented);
  add_string(*bulk_archive, "--scope", k_undocumented);
  add_json(*bulk_archive, k_undocumented);
  return bulk_archive;
}
} // namespace planar::cmd::handlers::annotate_cli
