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
  add_string(*bulk_dismiss, "--operation-id", k_undocumented);
  add_string(*bulk_dismiss, "--anchor-path", k_undocumented);
  add_int(*bulk_dismiss, "--plan", k_undocumented);
  add_int(*bulk_dismiss, "--task", k_undocumented);
  add_string(*bulk_dismiss, "--vendor", k_undocumented);
  add_string(*bulk_dismiss, "--tag", k_undocumented);
  add_string(*bulk_dismiss, "--scope", k_undocumented);
  add_json(*bulk_dismiss, k_undocumented);
  return bulk_dismiss;
}
} // namespace planar::cmd::handlers::annotate_cli
