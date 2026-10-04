/// @file sweep.cppm
/// @brief CLI declaration for `annotate sweep`.
export module planar.cmd.planar.handlers.annotate.sweep;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the sweep CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_sweep(CLI::App& annotate) -> CLI::App* {
  CLI::App* sweep = annotate.add_subcommand("sweep", "Sweep stale annotations (resolved/dismissed older than --since-days).");
  add_int_default(*sweep, "--since-days", "30", k_undocumented);
  add_string(*sweep, "--scope", k_undocumented);
  add_json(*sweep, k_undocumented);
  return sweep;
}
} // namespace planar::cmd::handlers::annotate_cli
