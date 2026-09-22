/// @file bulk_dismiss.cppm
/// @brief CLI declaration for `annotate bulk-dismiss`.
export module planar.cmd.planar.handlers.annotate.bulk_dismiss;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
export auto attach_bulk_dismiss(CLI::App& annotate) -> CLI::App* {
  CLI::App* bulk_dismiss = annotate.add_subcommand("bulk-dismiss", "Dismiss every active annotation matching the filter.");
  add_string(*bulk_dismiss, "--operation-id");
  add_string(*bulk_dismiss, "--anchor-path");
  add_int(*bulk_dismiss, "--plan");
  add_int(*bulk_dismiss, "--task");
  add_string(*bulk_dismiss, "--vendor");
  add_string(*bulk_dismiss, "--tag");
  add_string(*bulk_dismiss, "--scope");
  add_json(*bulk_dismiss);
  return bulk_dismiss;
}
} // namespace planar::cmd::handlers::annotate_cli
