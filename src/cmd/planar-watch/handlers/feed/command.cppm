/// @file command.cppm
/// @brief CLI declarations for the planar-watch feed family.
module;
export module planar.cmd.planar_watch.handlers.feed.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::feed_cli {
export auto add(CLI::App& root) -> void {
  // --- feed -----------------------------------------------------------------
  // Folded in from `surface.cpp`'s generated `k_path_0` at task 6613 (M11.1).
  // Was previously declared via `cliapp::apply_surface` below, alongside
  // `run`/`sync-events`/`run list`/`run show`; `dispatch.cpp` already
  // registers a real handler for it (`handlers::feed`), so this is a pure
  // declaration-site move, not a behavior change. Verified byte-identical
  // against the prior generated description before this edit landed.
  CLI::App* feed = root.add_subcommand(
      "feed", "One event per claim transition, action transition, or task status\n  change, in occurrence-time order. The "
              "default planar-watch\n  invocation routes here.\n\n  Without --follow: print the initial snapshot up to "
              "--limit\n  events (default 100), newest first.\n  With --follow: print the snapshot, then stream new events "
              "as\n  they appear. Tier-1 poll; --interval defaults to 1s.\n\n  --tail N: emit the most-recent N events on "
              "first call (the\n  journalctl -f -n idiom). --tail 0 or negative exits with\n  InvalidValue. Combined with "
              "--follow: the tail emission comes\n  first, then only NEW events stream (no re-emit of tailed events).\n\n  "
              "Filters (--vendor / --plan / --task / --since) narrow both the\n  snapshot and the streaming view.\n\n  "
              "--json emits NDJSON \xe2\x80\x94 one JSON object per line, no surrounding\n  array, no trailing comma. "
              "Consumers can pipe through `jq -c`.");
  cliapp::add_bool_flag(*feed, "--follow", "Stream new events until SIGINT");
  shared::add_vendor(*feed, "Vendor filter");
  shared::add_int(*feed, "--plan", "Plan id filter (matches plan-direct, task-on-plan, and plan_step-on-plan events)");
  shared::add_int(*feed, "--task", "Task id filter");
  feed->add_option("--since")->description("Only events with at >= this ISO8601 timestamp");
  shared::add_int(*feed, "--limit", "Snapshot row cap (default 100)");
  shared::add_int(*feed, "--tail", "Return only the most-recent N events (must be > 0)");
  cliapp::add_bool_flag(*feed, "--json", "Emit NDJSON");
  feed->add_option("--interval")->description("Poll interval for --follow (default 1s)");
}
} // namespace planar::cmd::watch::handlers::feed_cli
