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
  add_int_default(*sweep, "--since-days", "30", "Archive resolved or dismissed annotations older than this many days");
  add_string(*sweep, "--scope", "Restrict the sweep to this scope slug");
  add_json(*sweep, "Emit machine-readable JSON instead of text");
  return sweep;
}
} // namespace planar::cmd::handlers::annotate_cli
