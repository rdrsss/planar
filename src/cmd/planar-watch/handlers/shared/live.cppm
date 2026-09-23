/// @file live.cppm
/// @brief `planar.cmd.planar_watch.handlers.live` — the two verbs that
/// render the WIDE claim-column line: `ps` and `tree` (plan 996, tasks 6120
/// and 6039).
///
/// Port targets: zig/src/cmd/planar-watch/handlers/{ps,tree}.zig.
///
/// ## Why these two and not the other four
///
/// `claims`, `actions`, `plans` and `log` list rows. These two render a
/// STATE: for each claim they resolve the entity's storage scope, fetch the
/// newest action to fill the `activity:` column, and age the heartbeat
/// against the host clock. That per-row enrichment — three extra lookups
/// and a clock read — is the whole cost difference between the two groups,
/// and it is why the Zig tree extracted `format.zig` out of `ps.zig` the
/// moment `tree.zig` needed the same columns. Same split here.
///
/// ## `ps --stale` can list one claim TWICE, and that is the oracle
///
/// The `active` bucket comes from `list_active_claims_sorted`, which
/// filters on `status = 'active'` ALONE. The `stale` bucket includes
/// `status='active' AND lease_expires_at < now`. A claim whose lease has
/// passed but which `reconcile` has not swept yet satisfies both, so it
/// appears in both. See `agentactivity.cppm`'s note on
/// `list_active_claims_sorted`; reproduced (D2), not corrected.
///
/// ## `--follow` is DECLARED but REFUSED
///
/// Same call, same reasoning, as `planar.cmd.planar_watch.handlers.ledger`
/// — see that module's header. Exit 64, not a silent single-shot.
module;

export module planar.cmd.planar_watch.handlers.live;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch ps`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto ps(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-watch tree`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto tree(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
