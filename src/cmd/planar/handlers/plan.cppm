/// @file plan.cppm
/// @brief `planar.cmd.planar.handlers.plan` — the `planar plan create` leaf
/// (plan 996, task 6133).
///
/// Port target: zig/src/cmd/planar/handlers/plan/create.zig.
///
/// ## Why this verb is not "call the engine and print the row"
///
/// `engine_planning`'s `create_plan` was ported and unit-tested long
/// before this handler existed, which makes the verb look like pure
/// plumbing. It is not, and the part that is not plumbing is the part a
/// green unit test cannot see: the SCOPE the row lands in.
///
/// `create_plan` takes an OPTIONAL `scope` and writes `global` when it is
/// unset. The handler's `--scope` flag is also optional. Threading one
/// straight into the other compiles, exits 0, prints a plausible row —
/// and silently files every plan under `global` even when the operator is
/// sitting inside a project bound to an association. That is exactly the
/// bug zig/src/cmd/planar/handlers/plan/create.zig:20-25 cites plan 352
/// task 2450 for, and it is the same shape as task 6128's NULL
/// `repo_root` and task 6132's inherited-`$PWD` wrong `root_path`: an
/// omitted optional argument taking its default, invisibly.
///
/// So the handler does two things the engine cannot do for itself:
///
///   1. cwd-DERIVES the write scope when `--scope` is absent
///      (`planar.cmd.planar.scope`'s `resolve_write_scope`), so an
///      associated project's plans land on the association.
///   2. REFUSES, at exit 5, when the cwd matched a project that has no
///      association at all. Not a fallback to `global` — a refusal that
///      names the project and spells out both remedy commands. Falling
///      back would be the silent-wrong-row outcome again, just with a
///      friendlier face.
///
/// The refusal is deliberately gated on `--scope` being ABSENT: an
/// explicit `--scope global` is the operator overriding the rule on
/// purpose, and it is the escape hatch the refusal text advertises.
///
/// ## Two error paths, two different exit codes
///
/// An unparseable `--status` fails with `error: unknown status '<v>'` at
/// exit 1, while the sibling `assoc create`'s unparseable `--kind` fails
/// at exit 2. That asymmetry is not a typo on either side: zig's plan
/// handler dies with `error.InvalidStatus` (no arm in `exit.zig`'s
/// `codeFor`, so the generic 1 bucket) and the association handler with
/// `error.InvalidInput` (mapped to 2). Both were captured from the oracle
/// rather than reasoned about.
module;

export module planar.cmd.planar.handlers.plan;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar plan create <title> [flags]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) for an unknown
/// `--status` / an engine failure, `scope_mismatch` (exit 5) when the cwd
/// project has no association and no `--scope` was passed, or
/// `slug_conflict` (exit 6) on a slug collision.
export auto plan_create(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan show <plan-id> [--json]`.
///
/// The id positional is declared as a STRING, not an int, and that is
/// load-bearing: the oracle parses it itself so it can refuse with its own
/// message and its own exit code (`plan id must be an integer, got 'abc'`,
/// exit 2) rather than letting the parser emit a generic type error at
/// exit 1. Oracle-captured both ways.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `not_found` (exit 1) when no plan has that id.
export auto plan_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan list [--scope] [--status] [--parent] [--json]`.
///
/// Two things here are not plumbing:
///
///   1. `--status` takes a COMMA-SEPARATED list (`--status draft,active`),
///      splits on `,`, trims spaces, and skips empty tokens. A single
///      unparseable token refuses the whole call at exit 1. `task list`'s
///      `--status` is deliberately NOT a list — see `task_list`.
///   2. With no `--scope`, the listing is filtered by the cwd-derived READ
///      SET, which may hold more than one scope. An empty read set is a
///      REFUSAL, not an unfiltered listing.
///
/// `--touches` is declared but not implementable here — see the
/// implementation for the loud refusal and why it is not a silent
/// no-filter.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) for an unknown status /
/// an unresolvable scope / an empty read set, or `not_implemented`
/// (exit 64) for `--touches`.
export auto plan_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan update <plan-id> [--title] [--status] …`.
///
/// `--parent 0` is the documented CLEAR sentinel, not a parent whose id is
/// zero: the oracle maps `0` onto `clear_parent` and any other value onto a
/// reassignment. Passing it straight through as an id would write a
/// dangling foreign key.
///
/// Closing a plan (`--status done` / `--status abandoned`) that still has
/// open descendant plans emits a stderr ADVISORY and proceeds. It is not a
/// refusal, and the difference is deliberate in the oracle — see the
/// implementation.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id,
/// `generic_failure` (exit 1) for an unknown status / an illegal transition
/// / a parent cycle / no such plan, or `slug_conflict` (exit 6).
export auto plan_update(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan recompute-status (--plan <id> | --all) [--json]`.
///
/// Exactly one of `--plan` and `--all` is required; neither and both are
/// both refusals at exit 2. The engine is single-plan, so the `--all` walk
/// is the handler's own loop — a per-plan failure is reported to stderr and
/// SKIPPED rather than aborting the walk, matching the oracle.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) when the flag pair is wrong,
/// or `generic_failure` (exit 1) when a single `--plan` target does not exist.
export auto plan_recompute_status(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan step add <plan-id> <body> [--after <ordinal>] [--json]`.
///
/// `--after` sets the ordinal VERBATIM and renumbers nothing, so it
/// collides rather than inserting; see
/// `planar.engine.planning.plan_step`'s file header for the oracle
/// captures that establish this against the flag's name.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `generic_failure` (exit 1) when the plan does not exist or the ordinal
/// is taken.
export auto plan_step_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan step list <plan-id> [--json]`.
///
/// An unknown plan id lists EMPTY at exit 0 rather than refusing — the
/// oracle's behaviour, reproduced deliberately. See `list_steps`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id.
export auto plan_step_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan step done <step-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `generic_failure` (exit 1) when the step is missing or already terminal.
export auto plan_step_done(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan step skip <step-id> [--json]`.
///
/// Refuses with a DIFFERENT message from `done` on the shared
/// `invalid_transition` error (`cannot be skipped (must be pending)` vs
/// `is already terminal`). Both exit 1, so the distinction is only
/// observable byte-wise and is pinned as such.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `generic_failure` (exit 1) when the step is missing or not `pending`.
export auto plan_step_skip(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan step link <step-id> <task-id> [--json]`.
///
/// A missing step and a missing task produce the SAME message
/// (`step N or task M not found`), which is the oracle's; the handler does
/// not distinguish them.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `generic_failure` (exit 1) when either row is missing.
export auto plan_step_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan link <plan-id> <ref> --relationship <rel>
/// [--scope <s>] [--json]`.
///
/// One arm of the shared entity-link surface; the whole body lives in
/// `planar.cmd.planar.handlers.links::entity_link_verb`, which this
/// forwards to with this verb's subject kind, JSON key and arrow. See that
/// module's header for the three JSON envelopes, the zero-byte empty case
/// and why `--scope` is accepted but inert.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, an
/// absent `--relationship`, an unknown relationship or a malformed ref, or
/// `generic_failure` (exit 1) for a duplicate link or a missing endpoint.
export auto plan_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan descendants <plan-id> [--json]`. READ-ONLY.
///
/// ## THE EXISTENCE PROBE IS THE HANDLER'S OWN, AND IT MUST STAY
///
/// The oracle runs a `select count(*) from plans where id = ?` in the
/// HANDLER before calling the walk, and refuses with `no plan with id N` at
/// exit 1. `walk_tree` would also fail on an unknown anchor, but through its
/// title lookup and with a different error, so deleting the probe as
/// redundant would change the message. The probe is kept and the walk's
/// `not_found` arm is consequently unreachable from the CLI.
///
/// ## THE TWO OUTPUT SHAPES DISAGREE ABOUT WHAT A PLAN IS CALLED
///
/// JSON carries `kind` and `role` as SEPARATE fields, where `kind` collapses
/// the anchor/child distinction to `"plan"` and `role` preserves it
/// (`"anchor"` / `"child"` / `"task"`). Text carries one prefix string that
/// fuses them: `plan (anchor)`, `plan (child)`, `task`. So `role` is
/// `"task"` for a task while `kind` is also `"task"` -- the fields are
/// redundant on tasks and load-bearing only on plans.
///
/// The text row is `<prefix>:<id>  <title>` with TWO spaces; the JSON form
/// is a single-line array. Both end in exactly one newline, and an anchor
/// with no descendants still emits its own entry rather than an empty list.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `not_found` (exit 1) when the anchor plan does not exist.
export auto plan_descendants(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar plan next <plan-id> [--include-claimed]
/// [--include-stale] [--json]`. READ-ONLY (plan 996, task 6309).
///
/// Port target: `zig/src/cmd/planar/handlers/plan/next.zig` over
/// `agentactivity::next_work`, which landed with this leaf.
///
/// ## IT IS NOT `plan descendants` WITH A CLAIM COLUMN
///
/// The two verbs walk the graph DIFFERENTLY and legitimately disagree about
/// what a plan contains. `plan descendants` follows `entity_links`
/// `derives-from` edges out of the anchor; `next_work` follows
/// `plans.parent_plan_id` and then `tasks.plan_id`. A task whose plan is a
/// `parent_plan_id` child of the anchor but carries no `derives-from` edge
/// is listed HERE and invisible THERE, on the same database. That is a fact
/// about the two readings, not a bug in either, and `plan_next_leaf.t.cpp`
/// seeds a task that exercises exactly that gap so a future "unify the two
/// walks" cleanup fails a test instead of passing review.
///
/// ## THE TEXT FORM HIDES TWO BUCKETS AND THE JSON FORM NEVER DOES
///
/// `--include-claimed` / `--include-stale` gate the TEXT rows only. The
/// JSON object always carries all four arrays, and the HEADER line's
/// counts are always the true totals — so text output with neither flag
/// shows `claimed:2` in the header and zero claimed rows beneath it. That
/// reads like a bug and is the oracle's behaviour.
///
/// ## `done` IS A SEPARATE QUERY, NOT A FIFTH BUCKET
///
/// `next_work` drops `done` and `cancelled` rows entirely, so the summary's
/// `done` count cannot be tallied from them and comes from its own
/// `count(*)` over the same `plan_tree` CTE. Note the asymmetry that
/// follows: `cancelled` tasks are counted NOWHERE — not in a bucket, not in
/// `done`. Every other status is in exactly one place.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `not_found` (exit 1) when the plan does not exist.
export auto plan_next(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
