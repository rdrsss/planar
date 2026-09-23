/// @file command.cppm
/// @brief CLI declarations for the planar-watch log family.
module;
export module planar.cmd.planar_watch.handlers.log.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::log_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- log ----------------------------------------------------------------
  CLI::App* log = root.add_subcommand("log", "Streams the agent_actions + agent_work_claims history scoped to\n"
                                             "  one entity or one claim_token. Exactly one of\n"
                                             "  --task / --plan / --entity / --session / --claim is required.\n"
                                             "\n"
                                             "  Entries are emitted in occurrence-time order (oldest first)\n"
                                             "  as a discriminated union: each entry carries a `.kind` field\n"
                                             "  that is either `action` (full ActionRow payload) or\n"
                                             "  `claim_acquired` / `claim_heartbeat` / `claim_released` /\n"
                                             "  `claim_stale` (with ClaimRow payload).");
  shared::add_int(*log, "--task", "Filter to one task id");
  shared::add_int(*log, "--plan", "Filter to one plan id (matches entity_kind=plan rows)");
  log->add_option("--entity")->description("Filter to one entity, kind:id form");
  shared::add_int(*log, "--session", "Filter to one session_id");
  log->add_option("--claim")->description("Filter to one claim_token");
  shared::add_int(*log, "--limit", "Row cap (default 100)");
  shared::add_json(*log);
}
} // namespace planar::cmd::watch::handlers::log_cli
