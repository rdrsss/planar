/// @file resolve.cppm
/// @brief CLI declaration for `workbench resolve`.
export module planar.cmd.planar.handlers.workbench.resolve;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the resolve CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_resolve(CLI::App& workbench) -> CLI::App* {
  CLI::App* resolve = workbench.add_subcommand("resolve", "Settle a sync conflict by choosing FS or DB.");
  add_string_required(*resolve, "--prefer", "Which side to prefer (fs|db)");
  add_json(*resolve);
  add_positional(*resolve, "event-id");
  return resolve;
}
} // namespace planar::cmd::handlers::workbench_cli
