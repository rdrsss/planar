/// @file runs.cppm
/// @brief `planar.cmd.planar.handlers.runs` — all TEN `bench` and `run`
/// leaves (plan 996, tasks 6149 and 6362).
///
/// One module for two families because they are one table and one engine
/// bucket: `bench start|event|touch|harvest|finish|show` and `run start|
/// event|finish|show` all read and write `runs` / `run_events` /
/// `run_touches` through `planar.engine.runs.lifecycle`, and every byte
/// they print comes from `planar.engine.runs.render`. Splitting them would
/// duplicate the uid-resolution helper that is the only shared code either
/// family has.
///
/// `bench harvest`, the tenth leaf and the last to wire, landed at task
/// 6362 once `planar.engine.runs.harvest` existed to call — its own engine
/// half was deferred WITH its git-subprocess dependency at task 6095, and
/// the layer-1 `planar.git` seam that closed that gap (tasks 6128/6137)
/// did not exist yet either. See `src/engine/runs/CMakeLists.txt` for
/// the full account.
///
/// ## What this layer adds over the engine
///
/// Almost nothing, and that thinness is the design rather than a shortcut.
/// The engine already owns validation (`is_valid_terminal_status`,
/// `is_valid_json_payload`, `touch_kind_from_text`) AND every rendered
/// byte. What is left here is: read flags, resolve a `run_uid` to a row id,
/// map `runs_error` to this binary's exit-code buckets, and write the
/// renderer's output verbatim.
///
/// ## The exit-code buckets are NOT uniform across the family
///
/// Oracle-captured, and the differences are the reason each leaf is pinned
/// separately rather than through one shared mapper:
///
///     bench start <dup uid>          exit 6  (precondition_conflict)
///     bench event <dup seq>          exit 6
///     bench touch <dup tuple>        exit 1  (query_failed — a UNIQUE
///                                    violation the engine does NOT
///                                    special-case; see below)
///     bench|run show <missing uid>   exit 1  (not_found)
///     bench|run finish --status X    exit 2  (invalid_input)
///     bench start --plan <missing>   exit 1  (an FK failure surfacing as
///                                    query_failed, NOT a named refusal)
///     bench harvest --base w/o --head exit 2 (invalid_input, checked
///                                    BEFORE the database is opened)
///     bench harvest <missing uid>    exit 1  (not_found, same shared path
///                                    every other uid-taking leaf uses)
///     bench harvest <bad worktree>   exit 1  (generic_failure — the git
///                                    seam reports "no answer" for every
///                                    failure mode, so this is the ONLY
///                                    bucket a `git_failed` result can map
///                                    to; see harvest.cppm's own header)
///
/// ## Three lifecycle transitions are UNGUARDED, and that is reproduced
///
/// Probed against the oracle rather than assumed:
///
///     $Z bench finish b1 --status completed   -> exit 0
///     $Z bench finish b1 --status aborted     -> exit 0   (SECOND finish)
///     $Z bench event  b1 --kind post --seq 2  -> exit 0   (after finish)
///     $Z bench touch  b1 ... --kind actual    -> exit 0   (after finish)
///
/// A double finish overwrites `status` and re-stamps `ended_at`; an event
/// or touch after finish appends normally. There is no state machine in the
/// oracle and none is added here (D2). A port that "fixed" this would
/// refuse operations the reference binary accepts.
///
/// ## An oracle stderr artifact that is NOT reproduced
///
/// Every `query_failed` path on the reference binary prints TWO lines:
///
///     error: runs.touch exec failed: StepFailed
///     error: bench touch: QueryFailed
///
/// The first is a `std.log.err` from zig's db layer, and when stderr is a
/// FILE rather than a pipe it interleaves with the buffered `exit.die`
/// writer at a different offset and comes out mangled. Task 6135 already
/// settled this class as an oracle artifact and did not reproduce it (see
/// `handlers.t.cpp`'s task-6135 header); the same call is made here. Only
/// the `error: <leaf>: QueryFailed` line is emitted.
module;

export module planar.cmd.planar.handlers.runs;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar bench start` — mint a run and snapshot declared touches.
///
/// Prints the uid alone (no `--json` flag exists on this leaf). Warns on an
/// unrecognized `--arm` and still exits 0; the arm set is recognized, not
/// closed.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto bench_start(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar bench event` — append a journal row at a CALLER-supplied
/// `--seq`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto bench_event(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar bench touch` — record one declared or actual file touch.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto bench_touch(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar bench harvest` — diff a worktree and record the changed
/// paths as `kind='actual'` touches for a run/task.
///
/// `--base` and `--head` must appear together or not at all; supplying
/// exactly one refuses at `invalid_input` (exit 2) before the database is
/// even opened. Prints the count of distinct paths diffed.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto bench_harvest(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar bench finish` — set the run's terminal status.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto bench_finish(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar bench show` — the measurement view: header, events, AND
/// touches.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto bench_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar run start` — mint an operational run.
///
/// The arm is `--workflow`'s value, or the literal `"op"`. Output is JSON
/// unconditionally — the leaf declares `--json` but it changes nothing.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto run_start(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar run event` — append a journal row at an AUTO-INCREMENTED
/// seq (the difference from `bench event`).
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto run_event(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar run finish` — set the run's terminal status.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto run_finish(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar run show` — the operational view: header and events, with
/// NO touches (they are measurement-only).
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto run_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `bench` command tree on `root`.
///
/// The CLI declaration for every `bench` node, colocated with the
/// `bench_*` handlers above (plan 1051, M11.3d — decision 1068). No
/// `bench` node was ever hand-declared in `tree.cpp`, so all seven came
/// from `surface.cpp`'s generated table.
///
/// `bench` and `run` share this module because they share their engine,
/// but they are separate top-level groups at separate catalog positions
/// (`bench` 39th, `run` 41st) and `run` is folded by a later wave; this
/// function declares `bench` ONLY.
///
/// `start --task` is the surface's first REPEATABLE flag and the reason
/// `planar.cmd.planar.declare` grew `add_string_list`.
/// @param root The root app to attach the `bench` group to.

/// @brief Declare the `run` command tree on `root`.
///
/// The CLI declaration for every `run` node, colocated with the `run_*`
/// handlers above (plan 1051, M11.3e — decision 1068). None was
/// hand-declared in `tree.cpp`.
///
/// Separate from `declare_bench` on purpose. The two groups share this
/// module because they share an engine, but they are separate top-level
/// verbs at separate catalog positions (`bench` 39th, `run` 41st) with
/// separate descriptions, and one function declaring both would have to
/// be called twice from two places in the root's order.
/// @param root The root app to attach the `run` group to.

} // namespace planar::cmd::handlers
