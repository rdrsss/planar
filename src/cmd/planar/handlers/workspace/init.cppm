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
  add_string(*init, "--name", "Human-readable workspace name (default: the directory name)");
  add_string(*init, "--slug", "Workspace slug (default: derived from the directory name)");
  add_int_default(*init, "--scan", "1", "Directory levels to scan for child repos (default: 1)");
  add_bool(*init, "--meta-repo", "Treat the cwd git repository as a workspace container and member project");
  add_bool(*init, "--no-scan", "Skip the post-init pipeline (routing build, AGENTS.md regenerate, symlinks)");
  add_bool(*init, "--enrich", "Merge cached LLM enrichment results into the routing table; cannot combine with --no-scan");
  add_json(*init, "Emit machine-readable JSON instead of text");
  return init;
}
} // namespace planar::cmd::handlers::workspace_cli
