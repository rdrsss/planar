/// @file syncevents.cppm
/// @brief `planar.cmd.planar_watch.handlers.syncevents` — `sync-events`
/// (plan 1006, task 6448).
///
/// Port target: zig/src/cmd/planar-watch/handlers/syncevents.zig.
///
/// Reads `sync_events`, optionally joined through `external_links` /
/// `external_systems` for the `--plan` / `--system` / `--entity` filters.
/// `sync.cppm`'s existing `sync_event` / `events_for_link` shape is
/// per-LINK and lacks `link_id` and the multi-filter surface this verb
/// needs, so this module reads the table directly rather than growing that
/// one for a single, differently-shaped caller.
module;

export module planar.cmd.planar_watch.handlers.syncevents;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch sync-events`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto sync_events(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
