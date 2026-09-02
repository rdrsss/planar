/// @file closure.cppm
/// @brief `planar.cmd.planar.handlers.closure` — the `planar closure show`
/// leaf (plan 996, task 6189).
///
/// Port target: zig/src/cmd/planar/handlers/closure/show.zig.
///
/// One of the two `closure` leaves. `closure compute` is NOT here and stays a
/// declared exit-64 refusal: its engine half is blocked on tree-sitter (not
/// vendored), on a filesystem-corpus-walk seam that does not exist, and on a
/// layer-2 composition with `engine_planning` + `engine_identity`. All three
/// blockers, and the oracle transcript for whoever lands it, are recorded in
/// `planar.engine.closure.store`'s header. Wiring a handler over an absent
/// extractor would mean inventing symbol names and token weights; refusing by
/// name does not.
///
/// ## Why this leaf is worth its own file rather than a line in another
///
/// It is the cleanest instance in the tree of a read that CANNOT fail into a
/// not-found. `closure show 999` against a database with no task 999 is exit
/// 0 and `{"task_id":999,"rows":[]}` — the requested id is ECHOED back from
/// the argument, not read from a row, so the envelope names a task that does
/// not exist. That is the oracle's contract and it is pinned, because the
/// obvious "improvement" (refusing an unknown task) would break every caller
/// that polls a closure before computing it.
module;

export module planar.cmd.planar.handlers.closure;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar closure compute <task-id> [--scope S] [--json]`.
export auto closure_compute(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar closure show <task-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, an `invalid_input` (exit 2) for a non-integer positional,
/// or a `generic_failure` (exit 1) when the query fails.
export auto closure_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
