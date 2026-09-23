/// @file ledger.cppm
/// @brief `planar.cmd.planar_watch.handlers.ledger` — the four FLAT read
/// verbs: `claims`, `actions`, `plans` and `log` (plan 996, tasks 6120 and
/// 6039).
///
/// Port targets: zig/src/cmd/planar-watch/handlers/{claims,actions,plans,
/// log}.zig.
///
/// ## Why four leaves share one module
///
/// They share their whole shape: read rows through
/// `planar.engine.runtime.agentactivity`'s read paths, apply the same
/// `--vendor` / `--plan` predicates, and emit either a `generated_at`-
/// stamped JSON envelope or a `<noun>: <count>` text header followed by
/// one line per row. Splitting them across four module pairs would
/// duplicate the envelope and the filter in four places for no boundary
/// anyone can observe. `planar-agent`'s `handlers/claims` already groups
/// leaves this way.
///
/// The two verbs that do NOT share that shape — `ps` and `tree`, which
/// render the wide claim-column line and resolve a latest action per row —
/// live in `planar.cmd.planar_watch.handlers.live` instead.
///
/// ## `--follow` is DECLARED but REFUSED, deliberately
///
/// `claims`, `actions` and `plans` all declare `--follow` / `--interval` in
/// the tree, because `src/cmd/catalog_parity.hpp` compares this binary's
/// declared flag set against the oracle's and a missing flag is a
/// transcription failure it exists to catch. The streaming arm itself is
/// NOT ported: it is a SIGINT-driven poll loop over
/// `zig/src/cmd/planar-watch/handlers/follow.zig`, and this tree has no
/// signal handling of any kind.
///
/// So `--follow` returns `not_implemented` (exit 64) rather than emitting
/// one snapshot and exiting 0. That is the honest option of the two: a
/// silent single-shot would look like a working `--follow` to a script that
/// pipes it, and would be discovered as a hang that never happened rather
/// than as a refusal. Named here, and pinned by a test.
///
/// ## `--vendor` / `--plan` filter the ROWS but NOT the COUNT
///
/// The text header prints the row count BEFORE filtering, so
/// `claims --vendor codex` can print `claims: 2` above a single line. That
/// is the reference binary's behavior — the count comes off `rows.len` and
/// the filter is applied in the emit loop — and it is reproduced (D2)
/// rather than corrected. `handlers.t.cpp` pins it, so a later "fix" has to
/// be a deliberate decision rather than a silent one.
module;

export module planar.cmd.planar_watch.handlers.ledger;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch claims`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto claims(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-watch actions`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto actions(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-watch plans`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto plans(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-watch log`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto log(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
