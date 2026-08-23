/// @file recovery.cppm
/// @brief `planar.cmd.planar_agent.handlers.recovery` — the two operator
/// verbs for claims whose owner is gone: `reconcile` and `abort` (plan 996,
/// task 6038).
///
/// Port target: zig/src/cmd/planar-agent/handlers/{reconcile,abort}.zig.
///
/// ## These are the escape hatch, and they are shaped like one
///
/// Every other verb in this binary is guarded on ownership and liveness.
/// These two are deliberately not, because the situation they exist for is
/// precisely "the process holding this lease is never coming back":
///
///   `abort`     force-releases ONE claim by token, from ANY session, in
///               ANY state. No ownership check, no lease check.
///   `reconcile` sweeps EVERY claim whose lease has passed, returns their
///               tasks to `todo` where the claim owns that transition, and
///               abandons `workflow_runs` rows whose recorded pid is gone.
///
/// `--dry-run` on `reconcile` is not a courtesy: the sweep is destructive
/// across an entire database by default, and the dry-run form writes
/// NOTHING — not the mark-stale, not the task reset, not the orphan sweep
/// — while reporting the exact candidate set the applied form would take.
module;

export module planar.cmd.planar_agent.handlers.recovery;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent reconcile [flags]`.
///
/// Wraps both sweeps — claims and runs — in ONE transaction when applying,
/// and in none at all when `--dry-run`, so a partially-applied sweep is
/// not a state the database can reach.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto reconcile(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-agent abort --claim <token> [flags]`.
///
/// Opens the aborting session BEFORE the transaction (it is a read-or-
/// create on `sessions` that has nothing to do with the claim), then does
/// the force-release, the task recovery, and the audit action row inside
/// one transaction.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto abort(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
