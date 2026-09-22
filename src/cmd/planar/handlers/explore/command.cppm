/// @file command.cppm
/// @brief CLI declaration for `planar explore`.
export module planar.cmd.planar.handlers.explore.command;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {
export auto declare_explore(CLI::App& root) -> void {
  CLI::App* explore = root.add_subcommand(
      "explore", "Launch the interactive Planar cockpit.\n\n  Equivalent to invoking `planar` with no verb on a terminal. Use\n  "
                 "`planar explore` when you want to force-launch the cockpit by name,\n  or from a context where bare-invocation "
                 "detection may not fire.\n\n  --plan, --task, and --scope seed the initial focus.\n\n  Falls back to this help "
                 "text when stdout is not a TTY, when TERM=dumb,\n  when PLANAR_NO_TUI is set, or when --plain is passed.");
  add_string(*explore, "--plan", "Seed initial focus on this plan ID");
  add_string(*explore, "--task", "Seed initial focus on this task ID");
  add_string(*explore, "--scope", "Seed scope filter");
  add_bool(*explore, "--plain", "Fall back to help/usage instead of launching the cockpit");
}
} // namespace planar::cmd::handlers
