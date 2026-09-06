/// @file run.cppm
/// @brief `planar.cmd.planar_watch.handlers.run` — `run list` / `run show`
/// (plan 1006, task 6448).
///
/// Port target: zig/src/cmd/planar-watch/handlers/run.zig.
///
/// ## Two tables, one verb group
///
/// `run list` unions rows from TWO tables that have nothing to do with each
/// other beyond both describing "a run": `workflow_runs` (the context-plane
/// table an external `planar-execute` workflow writes via
/// `planar-agent run start/end`, source `"wf"`) and `runs` (the
/// bench/op-arm table `planar run start` writes, source `"op"`). `--arm`
/// selects `wf`, `op`, or the default `all`. `run show <id>` drills into
/// `workflow_runs` ONLY — an op-arm run has no `context_records` and is
/// read back via `planar run show <run_uid>` instead.
///
/// Both tables are read with hand-written SQL rather than through an
/// existing engine module: `planar.engine.runtime.workflowruns` exposes
/// only `start` / `find` / `end` (no list-by-filter, no get-by-id), and
/// `planar.engine.runs.lifecycle` owns the `runs` table's OWN read surface
/// (`show` / `show_by_uid`) but nothing that filters by plan/status the way
/// this verb needs. Duplicating that shape here — rather than growing
/// either module for one caller — matches the oracle's own layering: both
/// SQL statements live directly in `run.zig`, not behind a shared store.
module;

export module planar.cmd.planar_watch.handlers.run;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch run list`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto run_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-watch run show`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto run_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
