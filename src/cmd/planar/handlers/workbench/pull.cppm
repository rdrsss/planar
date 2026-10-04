/// @file pull.cppm
/// @brief CLI declaration for `workbench pull`.
export module planar.cmd.planar.handlers.workbench.pull;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the pull CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_pull(CLI::App& workbench) -> CLI::App* {
  CLI::App* pull = workbench.add_subcommand("pull", "Apply FS→DB changes; report DB→FS drift.");
  add_bool(*pull, "--verbose", k_undocumented);
  add_json(*pull, k_undocumented);
  add_positional(*pull, "plan", k_undocumented);
  return pull;
}
} // namespace planar::cmd::handlers::workbench_cli
