/// @file command.cppm
/// @brief CLI declarations for the planar-watch claims family.
module;
export module planar.cmd.planar_watch.handlers.claims.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::claims_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- claims -------------------------------------------------------------
  CLI::App* claims = root.add_subcommand("claims", "Returns claim rows from agent_work_claims. The default is\n"
                                                   "  --status active.\n"
                                                   "\n"
                                                   "  --status active : claim row is in 'active' state with an\n"
                                                   "                    unexpired lease (default).\n"
                                                   "  --status stale  : status='stale' OR an expired-lease active\n"
                                                   "                    claim (matches `ps --stale`).\n"
                                                   "  --status all    : every row (active, released, completed,\n"
                                                   "                    aborted, stale) \xe2\x80\x94 the full claim ledger.");
  shared::add_vendor(*claims, "Filter by vendor");
  shared::add_int(*claims, "--plan", "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)");
  claims->add_option("--status")->description("active (default) | stale | all");
  shared::add_json(*claims);
  shared::add_follow(*claims, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s)");
}
} // namespace planar::cmd::watch::handlers::claims_cli
