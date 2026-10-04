/// @file command.cppm
/// @brief CLI declaration for `planar explore`.
export module planar.cmd.planar.handlers.explore.command;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
/// @brief Provide the declare explore command operation.
/// @param root Input root.
export auto declare_explore(CLI::App& root) -> void {
  CLI::App* explore =
      root.add_subcommand("explore", "Launch Planar Explorer.\n\n  Planar Explorer is a separate project. This command is "
                                     "reserved for\n  launching it and is not yet implemented: it prints this help text and\n  "
                                     "exits 0.\n\n  --plan, --task, and --scope are reserved to seed the initial focus.");
  add_string(*explore, "--plan", "Seed initial focus on this plan ID");
  add_string(*explore, "--task", "Seed initial focus on this task ID");
  add_string(*explore, "--scope", "Seed scope filter");
  add_bool(*explore, "--plain", "Print this help page (currently the only behavior)");
}
} // namespace planar::cmd::handlers
