/// @file command.cppm
/// @brief CLI declarations for the planar-watch actions family.
module;
export module planar.cmd.planar_watch.handlers.actions.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::actions_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- actions ------------------------------------------------------------
  CLI::App* actions =
      root.add_subcommand("actions", "Returns agent_actions rows ordered by started_at descending.\n"
                                     "\n"
                                     "  --kind     : action_kind filter (coder, reviewer, tool_call, etc.).\n"
                                     "  --entity   : restrict to one entity, `kind:id` form (e.g. `task:42`).\n"
                                     "  --plan     : restrict to actions on the plan, or on tasks/plan_steps belonging to it.\n"
                                     "  --task     : restrict to actions whose entity_kind=task, entity_id=N.\n"
                                     "  --vendor   : vendor filter.\n"
                                     "  --limit    : cap row count (default 100).");
  shared::add_vendor(*actions, "Vendor filter");
  actions->add_option("--kind")->description("action_kind filter");
  actions->add_option("--entity")->description("Restrict to one entity, kind:id form");
  shared::add_int(*actions, "--plan", "Filter by plan id");
  shared::add_int(*actions, "--task", "Filter by task id");
  shared::add_int(*actions, "--limit", "Row cap (default 100)");
  shared::add_json(*actions, "Emit machine-readable JSON instead of text");
  shared::add_follow(*actions, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s)");
}
} // namespace planar::cmd::watch::handlers::actions_cli
