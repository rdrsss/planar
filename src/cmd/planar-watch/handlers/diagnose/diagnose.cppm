/// @file diagnose.cppm
/// @brief `planar.cmd.planar_watch.handlers.diagnose` -- `planar-watch diagnose`
/// (plan 1132, task 7373).
///
/// Composes `planar.engine.diagnose` over the connection `planar-watch` already
/// holds: opened `SQLITE_OPEN_READONLY`, never written. The verb takes the evaluation
/// instant from the clock and hands it to the engine.
///
/// Exit mapping (spec 689 § Output contract, question 1046):
///   - 0 whenever the run completed, whatever it found (`ok` or `partial`).
///   - 2 for bad input: `--days` below 1, an unknown `--check` or plan, an
///     invalid instant. Nothing is printed to stdout.
///   - 1 otherwise. A diagnosis that ended `unavailable` (busy past 250 ms, a failed
///     query, a missing planning table) did not complete: it is printed
///     (`diagnose: unavailable (<reason>)`, or the JSON object) and the verb exits 1.
///     A database that cannot be opened keeps the binary's usual mapping (7 for a
///     schema mismatch).
module;

export module planar.cmd.planar_watch.handlers.diagnose;

import std;
import planar.cliapp.args;
import planar.engine.diagnose;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch diagnose`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto diagnose(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `diagnose` over a caller-supplied catalog: the seam tests use to put findings, partial
/// coverage and failing checks behind the verb's exit mapping while the shipped catalog is small.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param cat The catalog to evaluate.
/// @return Success, or the failure to report.
export auto diagnose_with_catalog(context& ctx, const cliapp::parsed_args& args, const engine::diagnose::catalog& cat)
    -> handler_result;

} // namespace planar::cmd::watch::handlers
