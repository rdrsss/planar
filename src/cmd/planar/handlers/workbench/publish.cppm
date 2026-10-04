/// @file publish.cppm
/// @brief CLI declaration for `workbench publish`.
export module planar.cmd.planar.handlers.workbench.publish;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::workbench_cli {
/// @brief Register the publish CLI node.
/// @param workbench Input workbench.
/// @return Registered CLI node.
export auto attach_publish(CLI::App& workbench) -> CLI::App* {
  CLI::App* publish = workbench.add_subcommand("publish", "Render and push workbench files to external system.");
  add_string_required(*publish, "--system", k_undocumented);
  add_json(*publish, k_undocumented);
  add_positional(*publish, "plan", k_undocumented);
  return publish;
}
} // namespace planar::cmd::handlers::workbench_cli
