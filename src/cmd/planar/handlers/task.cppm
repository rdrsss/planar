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

} // namespace planar::cmd::handlers
