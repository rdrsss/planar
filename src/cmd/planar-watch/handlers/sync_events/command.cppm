/// @file command.cppm
/// @brief CLI declarations for the planar-watch sync-events family.
module;
export module planar.cmd.planar_watch.handlers.sync_events.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::sync_events_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- sync-events ----------------------------------------------------
  CLI::App* sync_events = root.add_subcommand(
      "sync-events", "Returns sync_events rows ordered by `at` descending.\n\n  --plan     : restrict to events whose link "
                     "belongs to the given plan id.\n  --system   : restrict to events via a link on the given external "
                     "system slug.\n  --entity   : restrict to events via a link on one entity, `kind:id` form.\n  "
                     "--outcome  : filter by outcome value (ok, conflict, error, noop, \xe2\x80\xa6).\n  --since    : only "
                     "return rows with `at` >= this ISO8601 timestamp.\n  --limit    : cap row count (default 100).");
  shared::add_int(*sync_events, "--plan", "Filter by plan id");
  sync_events->add_option("--system")->description("Filter by external system slug");
  sync_events->add_option("--entity")->description("Filter by entity, kind:id form (e.g. task:42)");
  sync_events->add_option("--outcome")->description("Filter by outcome (ok, conflict, error, noop, \xe2\x80\xa6)");
  sync_events->add_option("--since")->description("Only rows at >= this ISO8601 timestamp");
  shared::add_int(*sync_events, "--limit", "Row cap (default 100)");
  shared::add_json(*sync_events);
}
} // namespace planar::cmd::watch::handlers::sync_events_cli
