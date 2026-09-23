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
  add_string(*bulk_resolve, "--operation-id");
  add_string(*bulk_resolve, "--anchor-path");
  add_int(*bulk_resolve, "--plan");
  add_int(*bulk_resolve, "--task");
  add_string(*bulk_resolve, "--vendor");
  add_string(*bulk_resolve, "--tag");
  add_string(*bulk_resolve, "--scope");
  add_json(*bulk_resolve);
  return bulk_resolve;
}
} // namespace planar::cmd::handlers::annotate_cli
