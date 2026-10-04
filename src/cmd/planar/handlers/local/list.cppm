/// @file list.cppm
/// @brief CLI declaration for `local list`.
export module planar.cmd.planar.handlers.local.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::local_cli {
/// @brief Register the list CLI node.
/// @param local Input local.
/// @return Registered CLI node.
export auto attach_list(CLI::App* local) -> CLI::App* {
  CLI::App* list =
      local->add_subcommand("list", "List projected skills and agents with their state: live, stale, missing, broken or legacy.");
  add_string(*list, "--vendor", "Only list projections for this vendor: claude, codex, copilot, gemini, antigravity, opencode");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::local_cli
