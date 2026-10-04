/// @file init.cppm
/// @brief CLI declaration for `workspace init`.
export module planar.cmd.planar.handlers.workspace.init;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
/// @brief Register the init CLI node.
/// @param workspace Input workspace.
/// @return Registered CLI node.
export auto attach_init(CLI::App* workspace) -> CLI::App* {
  CLI::App* init = workspace->add_subcommand("init", "Initialize a workspace (org-level association).");
  add_string(*init, "--name", k_undocumented);
  add_string(*init, "--slug", k_undocumented);
  add_int_default(*init, "--scan", "1", k_undocumented);
  add_bool(*init, "--meta-repo", k_undocumented);
  add_bool(*init, "--no-scan", k_undocumented);
  add_bool(*init, "--enrich", k_undocumented);
  add_json(*init, k_undocumented);
  return init;
}
} // namespace planar::cmd::handlers::workspace_cli
