/// @file task.cppm
/// @brief `planar.cmd.planar.handlers.task` — the `planar task add` leaf
/// (plan 996, task 6135).
///
/// Port target: zig/src/cmd/planar/handlers/task/add.zig.
///
/// ## Seven optional arguments, and the handler is the only thing that
/// carries them
///
/// `engine.planning.task`'s `create_task` was ported and unit-tested a
/// cycle before this handler existed, which makes the verb look like
/// plumbing. What makes it not plumbing is arity: `--body`, `--slug`,
/// `--plan`, `--parent`, `--priority`, `--next-action`, `--due` and
/// `--scope` are ALL optional on both sides, and `task_create_args`
/// default-constructs every one of them to a value that is individually
/// plausible. Drop any single one on the floor and the verb still
/// compiles, still exits 0, still prints a well-formed row — with a NULL
/// column where the operator put a value. That is the exact failure shape
/// task 6128 (NULL `repo_root`), task 6132 (inherited `$PWD`) and task
/// 6133 (`plan create` filing everything under `global`) each shipped and
/// had to be caught after the fact. The handler tests here therefore
/// assert the resulting `tasks` ROW, column by column, with `typeof()`
/// separating NULL from `''` — not the exit code and not stdout.
///
/// ## `task add` does NOT refuse an unassociated project. `plan create` does.
///
/// The sibling verb wired in task 6133 refuses at exit 5 when cwd-derive
/// comes back `project_unassociated`, because filing a plan under `global`
/// from inside an associated repo is a silent wrong row. `task add` shares
/// the resolver call and shares NONE of the refusal: the oracle threads
/// `resolution.scope` (unset -> the engine's `global` default) straight
/// through and exits 0. Verified by running it, not by reading it — inside
/// a registered-but-unassociated project the oracle writes
/// `scope_kind='global'`, `scope_id=NULL` and prints `scope:       global`.
/// Copying `plan create`'s refusal across because the two verbs look alike
/// would break every one of those calls. The asymmetry lives in the ORACLE
/// (zig/src/cmd/planar/handlers/plan/create.zig has the
/// `project_unassociated` arm; task/add.zig does not) and is preserved,
/// not harmonised.
///
/// ## The `--editor` path is DEFERRED WITH ITS DEPENDENCY, and refuses
///
/// The oracle opens `$EDITOR` for the body when, and only when, all three
/// of: `--body` was absent, `--editor` is on (it defaults to TRUE), and
/// stdout is a TTY. Process spawning does not exist anywhere in this tree
/// — it is the same missing seam that deferred `workflow run`, `bench
/// harvest` and `capture commits`. This handler therefore REFUSES on that
/// exact three-way conjunction rather than quietly creating a body-less
/// task, which would be a silent divergence on the one path an interactive
/// operator actually takes. Under ctest, under the integration harness and
/// under any pipe, stdout is not a TTY and the branch is unreachable, so
/// the deferral costs no coverage.
///
/// The TTY test is `&ctx.out() == &std::cout && isatty(...)`, not `isatty`
/// alone: a Catch2 test builds a `context` over a `std::ostringstream`
/// while the test binary's own stdout may well be a terminal, and the
/// bare check would drag every in-process test into the refusal.
module;

export module planar.cmd.planar.handlers.task;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar task add <title> [flags]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `slug_conflict` (exit 6) on a `--slug` collision,
/// `scope_mismatch` (exit 5) when the ambiguous-meta-workspace resolver
/// refuses, `invalid_input` (exit 2) when the title is missing, or
/// `generic_failure` (exit 1) for an unknown `--scope`, a dangling
/// `--plan` / `--parent`, or the deferred editor path.
export auto task_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task show <task-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `not_found` (exit 1) when no task has that id.
export auto task_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task packet <task-id> [--json]` (plan 996, task
/// 6324).
///
/// Compiles the authoritative current routing packet for a task. This is the
/// verb the ORCHESTRATOR reads to decide whether a task is dispatchable, so
/// two things about its result shape are contract rather than presentation:
///
///   - **An unready packet is exit 0.** `ready: false` with a populated
///     `reasons` array is a successful answer to the question asked, not a
///     failure. The only non-zero exits are a non-integer id (2) and an
///     unknown task (1). A caller that treats a refusal as an error would
///     stop for the wrong reason and lose the reasons list.
///   - **An unknown task REFUSES rather than emitting an empty packet.**
///     Returning a well-formed all-empty packet for a task that does not
///     exist would read as "not ready yet" instead of "you asked about
///     nothing", which is the worse failure of the two.
///
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success (including for an unready packet), `invalid_input`
/// (exit 2) for a non-integer id, or `not_found` (exit 1) when no task has
/// that id.
export auto task_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task list [--scope] [--status] [--plan]
/// [--priority-max] [--json]`.
///
/// Two divergences from the sibling `plan list` are the oracle's, not
/// oversights, and both were confirmed by running it:
///
///   - `--status` here takes ONE value, not a comma-separated list.
///     `--status todo,doing` fails as an unknown status.
///   - `--scope` here is NOT comma-split either; the whole raw string is
///     handed to the engine as one slug, so `--scope a,b` reports
///     `SlugNotFound`.
///
/// With no `--scope`, the listing is filtered by the cwd-derived READ SET,
/// which may hold several scopes. With no `--status`, the engine defaults to
/// the three OPEN statuses (todo/doing/blocked) — a done task is invisible
/// to a bare `task list`, by design.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) for an unknown status /
/// an unresolvable scope / an empty read set, or `not_implemented`
/// (exit 64) for `--touches`.
export auto task_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task facts stage <task-id> [--json]`.
///
/// Stages the task's routing facts under `operator-v1` provenance and then
/// reports the resulting packet, because "did the write succeed" is not the
/// operator's question — "is this task dispatchable now" is.
///
/// An unresolvable citation exits non-zero and prints one diagnostic per
/// offending artifact; nothing is staged in that case.
/// @param ctx Handler context.
/// @param args Parsed arguments.
/// @return Success, or the domain error.
export auto task_facts_stage(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task update <task-id> [--title] [--status] …`.
///
/// Carries TWO guards the other task verbs do not, in this order:
///
///   1. The cross-scope write guard — the task's stored scope must agree
///      with the operator's resolved write scope, else exit 5 naming both
///      and the `--scope` that would allow it.
///   2. The active-work-claim guard, but only when `--status` is present
///      and `--force` is not. A non-status patch on a claimed task is
///      allowed; oracle-confirmed (`task update <claimed> --title X` exits
///      0).
///
/// `--plan 0` is the CLEAR sentinel, mirroring `plan update`'s `--parent 0`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2), `scope_mismatch` (exit 5),
/// `slug_conflict` (exit 6), or `generic_failure` (exit 1).
export auto task_update(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task done <task-id> [--force] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// `generic_failure` (exit 1) for an active claim without `--force`, an
/// illegal transition, or no such task.
export auto task_done(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task cancel <task-id> [--json]`.
///
/// Note the missing `--force`: this verb has no override at all, and its
/// claim refusal therefore renders as the GENERIC `task cancel: TaskClaimed`
/// rather than the two-line advisory its siblings print. That is the
/// oracle's own asymmetry — `cancel.zig` has no `error.TaskClaimed` arm, so
/// the error falls to the catch-all `else`. Captured, not inferred.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2), or `generic_failure` (exit 1).
export auto task_cancel(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task block <task-id> --on <id> [--reason] [--force]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id or a
/// missing `--on`, or `generic_failure` (exit 1).
export auto task_block(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task reopen <task-id> --reason <…> [--status]`.
///
/// `--reason` is REQUIRED here even though it is declared optional, and the
/// refusal is the handler's (exit 2, `--reason is required for reopen`).
/// `--status` defaults to `todo`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2), or `generic_failure` (exit 1).
export auto task_reopen(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task touches add <task-id> <repo-slug> [--path <p>] [--json]`.
///
/// Without `--path` this writes only the coarse `entity_links` `task ->
/// repo touches` edge, and a pre-existing edge is an ERROR. With `--path`
/// it writes the edge AND a `task_touch_paths` row, treating a
/// pre-existing edge as a no-op — a path-touch implies the repo-touch.
/// The two writes commit atomically; a half-applied pair would make the
/// parallelizability rules fall back to the coarse whole-repo signal and
/// silently serialize a task that should be eligible.
///
/// Link verbs are documented UNGUARDED (CLAUDE.md § cross-scope guard), so
/// no scope guard runs and `--scope` is accepted and ignored, as in the
/// oracle.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer task id,
/// or `generic_failure` (exit 1) for an unknown repo slug, a missing task,
/// or an already-present edge in the no-`--path` mode.
export auto task_touches_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task touches list <task-id> [--json]`.
///
/// Lists both levels: the repo edges and the path declarations, each
/// ordered by repo slug. An unknown task id lists EMPTY at exit 0 — the
/// oracle performs no existence check and neither does this.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer task id.
export auto task_touches_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task touches infer <task-id> [--repo <slug>]
/// [--apply] [--wide] [--json]`.
///
/// PREVIEW BY DEFAULT: without `--apply` nothing is written. That is
/// load-bearing rather than a nicety — per decision 906 the two error
/// directions are asymmetric (over-declaring costs recoverable throughput,
/// under-declaring costs correctness at fan-in) and inference cannot tell
/// which it produced. Only the operator can, so it proposes and stops.
///
/// `--apply` writes only the writable classifications, and only when at
/// least one exists: applying to a task whose every candidate is held back
/// writes NOTHING, not even the coarse repo edge. `"applied"` in the JSON
/// still reports `true` in that case — it mirrors the flag, not the effect.
/// Both behaviours are oracle-captured.
///
/// Link verbs are documented UNGUARDED (CLAUDE.md § cross-scope guard), and
/// this one takes no `--scope` at all, so no guard runs.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer task id or
/// a repo registered with no `root_path`, or `generic_failure` (exit 1) for
/// a missing task, an unknown `--repo` slug, or a cwd inside no registered
/// checkout.
export auto task_touches_infer(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task touches remove <task-id> <repo-slug> [--path <p>] [--json]`.
///
/// DELIBERATELY not symmetric with `add`: `--path` withdraws one
/// `task_touch_paths` row and LEAVES the repo edge, and removing the repo
/// edge leaves any path rows in place. Both halves of that asymmetry are
/// the oracle's, are documented in the verb's own help text, and are
/// asserted directly — orphaned path rows keep driving eligibility after
/// their edge is gone, so "remove the edge" is not a way to withdraw a
/// path claim.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer task id,
/// or `generic_failure` (exit 1) for an unknown repo slug, an undeclared
/// path, or an absent edge.
export auto task_touches_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task link <task-id> <ref> --relationship <rel>
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
export auto task_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `task` command tree on `root`.
///
/// The CLI declaration for every `task` node, colocated with the handlers
/// above (plan 1051, M11.3a — decision 1068's one-declaration-site-per-node
/// target, sited per task 6401 next to the handler rather than in a second
/// central file). `planar.cmd.planar.tree` calls this while building the
/// root app.
///
/// The order of the `add_subcommand` calls IS the order the `--help` page
/// and the `schema` catalog list the children in; every group node sets
/// `require_subcommand(0)` so a bare group renders help at exit 0.
/// @param root The root app to attach the `task` group to.
export auto declare_task(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
