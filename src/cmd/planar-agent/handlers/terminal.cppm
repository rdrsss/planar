/// @file terminal.cppm
/// @brief `planar.cmd.planar_agent.handlers.terminal` — the four verbs that
/// END a work session: `complete`, `fail`, `release`, `block` (plan 996,
/// task 6038).
///
/// Port target: zig/src/cmd/planar-agent/handlers/{complete,fail,release,
/// block}.zig plus `terminal_common.zig`.
///
/// ## Exactly one of these runs, exactly once, per claim
///
/// That is the ritual's closing move, and the reason all four are here
/// together: they emit the SAME envelope, take the same `--claim` and
/// `--no-locality-probe` flags, and differ only in the pair of statuses
/// they write and the outcome they stamp on the claim's open actions.
///
///   verb       task ->     claim ->      actions ->   `--reason`
///   complete   done        completed     ok           (none; `--summary`)
///   fail       todo        aborted       error        REQUIRED
///   release    todo        released      aborted      optional
///   block      blocked     released      aborted      optional
///
/// `fail` and `release` differ ONLY in the claim status and the action
/// outcome, and that difference is the whole point: `released` records a
/// choice to stop, `aborted` records something going wrong. A sweep over
/// `failure_category` counts one and not the other.
///
/// ## What the handler does NOT do
///
/// `terminal_common.collectCommits` — the best-effort harvest of commits
/// made during the claim window — is not ported. It rests on
/// `sessioncommits.recordClaimWindowBestEffort`, a git revision walk
/// through `std.process.run`; the same dependency `capture commits` and
/// `bench harvest` were already deferred with. It runs OUTSIDE the
/// transaction in the original and writes only to `session_commits`, so
/// nothing atomic and nothing these verbs print depends on it.
/// `--no-locality-probe` is still declared on all four verbs, because
/// dropping a flag changes the `--help` bytes; it currently gates nothing
/// in this port and that is stated here rather than left to be discovered.
module;

export module planar.cmd.planar_agent.handlers.terminal;

import std;
import planar.cli;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent complete --claim <token> [--summary <text>]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto complete(context& ctx, const cli::match_result& args) -> handler_result;

/// @brief Handle `planar-agent fail --claim <token> --reason <text> [--category <c>]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto fail(context& ctx, const cli::match_result& args) -> handler_result;

/// @brief Handle `planar-agent release --claim <token> [--reason <text>]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto release(context& ctx, const cli::match_result& args) -> handler_result;

/// @brief Handle `planar-agent block --claim <token> --blocker <id> [--reason <text>]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto block(context& ctx, const cli::match_result& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
