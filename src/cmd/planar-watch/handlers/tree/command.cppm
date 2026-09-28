/// @file command.cppm
/// @brief CLI declarations for the planar-watch tree family.
module;
export module planar.cmd.planar_watch.handlers.tree.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::tree_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- tree ---------------------------------------------------------------
  CLI::App* forest =
      root.add_subcommand("tree", "Walks agent_actions.parent_action_id chains and renders the\n"
                                  "  orchestrator \xe2\x86\x92 sub-agent forest. Root rows have parent_action_id IS NULL.\n"
                                  "  Each child is indented with unicode tree characters (\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 / "
                                  "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 / \xe2\x94\x82).\n"
                                  "\n"
                                  "  --root-session <id>  scope to one session's subtree (error if unknown).\n"
                                  "  --follow             stream; re-renders on WAL change (Tier-2 wake).\n"
                                  "  --interval           maximum poll cadence for --follow (default 1s).\n"
                                  "\n"
                                  "  Each row shows the claim's: scope vendor activity worktree branch last_hb.");
  shared::add_int(*forest, "--root-session", "Scope output to one session's subtree (session id)");
  shared::add_follow(*forest, "Stream re-renders until SIGINT", "Poll interval for --follow (default 1s; e.g. 100ms)");
}
} // namespace planar::cmd::watch::handlers::tree_cli
