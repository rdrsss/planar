/// @file push.cppm
/// @brief CLI declaration for `workbench push`.
export module planar.cmd.planar.handlers.workbench.push;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
export auto attach_push(CLI::App& workbench) -> CLI::App* {
  CLI::App* push = workbench.add_subcommand("push", "Apply DB→FS changes atomically; report FS→DB drift.");
  add_bool(*push, "--verbose");
  add_json(*push);
  add_string(*push, "--filter-mode", "Terminal-status filter: 'failures' (default) or 'all'");
  add_bool(*push, "--apply-cleanup", "Remove pre-existing FS files for entities this push would have filtered");
  add_positional(*push, "plan");
  return push;
}
} // namespace planar::cmd::handlers::workbench_cli
