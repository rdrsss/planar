/// @file command.cppm
/// @brief CLI declarations for the planar-watch plans family.
module;
export module planar.cmd.planar_watch.handlers.plans.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::plans_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- plans --------------------------------------------------------------
  CLI::App* plans = root.add_subcommand("plans", "Each row pairs a plan with its in-flight summary:\n"
                                                 "    active_claims  \xe2\x80\x94 claims with status='active' and\n"
                                                 "                     lease_expires_at >= now() targeting any task\n"
                                                 "                     under the plan.\n"
                                                 "    active_actions \xe2\x80\x94 agent_actions rows with ended_at IS NULL\n"
                                                 "                     whose entity_kind/entity_id refer to a task\n"
                                                 "                     under the plan.\n"
                                                 "    last_event_at  \xe2\x80\x94 max of claim claimed_at / heartbeat /\n"
                                                 "                     released_at and action started_at /\n"
                                                 "                     ended_at across the plan's tasks; null\n"
                                                 "                     when no events recorded.\n"
                                                 "\n"
                                                 "  --in-flight-only drops plans where active_claims=0 AND\n"
                                                 "  active_actions=0.");
  cliapp::add_bool_flag(*plans, "--in-flight-only", "Skip plans with no live work");
  shared::add_json(*plans, cliapp::k_undocumented);
  shared::add_follow(*plans, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s)");
}
} // namespace planar::cmd::watch::handlers::plans_cli
