/// @file init.cppm
/// @brief CLI declaration for `workspace init`.
export module planar.cmd.planar.handlers.workspace.init;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::workspace_cli {
export auto attach_init(CLI::App* workspace) -> CLI::App* {
  CLI::App* init = workspace->add_subcommand("init", "Initialize a workspace (org-level association).");
  add_string(*init, "--name");
  add_string(*init, "--slug");
  add_int_default(*init, "--scan", "1");
  add_bool(*init, "--meta-repo");
  add_bool(*init, "--no-scan");
  add_bool(*init, "--enrich");
  add_json(*init);
  return init;
}
} // namespace planar::cmd::handlers::workspace_cli
