/// @file command.cppm
/// @brief CLI declarations for the planar-watch ps family.
module;
export module planar.cmd.planar_watch.handlers.ps.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::ps_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- ps -----------------------------------------------------------------
  CLI::App* ps = root.add_subcommand("ps", "Lists every currently active agent claim \xe2\x80\x94 one row per claim_token.\n"
                                           "\n"
                                           "  --stale also includes claims whose lease has expired OR whose\n"
                                           "  status is `stale` (set by `planar-agent reconcile`).\n"
                                           "\n"
                                           "  --vendor / --plan narrow the result.\n"
                                           "\n"
                                           "  --sort-by heartbeat (default) orders by most-recently-heartbeated\n"
                                           "  first. --sort-by lease restores the pre-M3 claimed_at ordering.\n"
                                           "\n"
                                           "  --follow turns the snapshot into a streaming view (Tier-1 poll;\n"
                                           "  --interval defaults to 1s). Exits 0 on SIGINT.");
  shared::add_vendor(*ps, "Filter by vendor (claude, codex, copilot, ...)");
  shared::add_int(*ps, "--plan", "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)");
  cliapp::add_bool_flag(*ps, "--stale", "Include stale + lease-expired claims");
  shared::add_json(*ps, cliapp::k_undocumented);
  shared::add_follow(*ps, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s; e.g. 100ms)");
  ps->add_option("--sort-by")->description("Sort order for active claims: heartbeat (default) or lease");
  ps->add_option("--group-by")->description("Group claims by dimension: role, scope, or vendor");
}
} // namespace planar::cmd::watch::handlers::ps_cli
