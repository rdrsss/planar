/// @file closure.cppm
/// @brief `planar.cmd.planar.handlers.closure` — the `planar closure show`
/// leaf (plan 996, task 6189).
///
/// Port target: zig/src/cmd/planar/handlers/closure/show.zig.
///
/// Both `closure` leaves are here. `closure compute` was ORIGINALLY a
/// declared exit-64 refusal — its engine half needed tree-sitter (not yet
/// vendored), a filesystem-corpus-walk seam that did not exist, and a
/// layer-2 composition with `engine_planning` + `engine_identity` — and this
/// paragraph used to say so. All three blockers are gone: tree-sitter is
/// vendored, the corpus walk landed as `planar.engine.closure.compute`, and
/// this handler composes it with the same two peers named above (see that
/// module's header for the full oracle transcript). `closure_compute` below
/// is a real, wired handler, not a placeholder.
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
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success (rendered summary or `--json` envelope), an
/// `invalid_input` (exit 2) for a non-integer positional or a task with no
/// `task_touch_paths` seeds, a `not_found` (exit 1) for an unknown task id,
/// a `scope_mismatch` when `--scope` disagrees with the task's own scope, or
/// a `generic_failure` (exit 1) when the extraction query fails.
export auto closure_compute(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar closure show <task-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, an `invalid_input` (exit 2) for a non-integer positional,
/// or a `generic_failure` (exit 1) when the query fails.
export auto closure_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
