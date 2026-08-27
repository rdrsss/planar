/// @file workflow.cppm
/// @brief `planar.cmd.planar.handlers.workflow` — the `planar workflow
/// list` and `planar workflow show` leaves (plan 996, task 6105).
///
/// Port target: zig/src/cmd/planar/handlers/workflow/{list,show}.zig.
///
/// These two are in the proving subset for three reasons, each of which
/// nothing else in the subset covers:
///
/// 1. THE TERMINATOR CONTRACT AT ITS SHARPEST. `workflow list --json`
///    emits NDJSON, and on an empty catalog it emits ZERO BYTES — not
///    `[]`, not a bare newline (oracle-captured; see
///    `planar.engine.workflows.render`'s header). A handler that appended
///    a newline to renderer output would break parity here in a way no
///    renderer change could fix. This is the leaf that makes "write the
///    payload verbatim" testable rather than aspirational, and it is
///    directly contrasted in `handlers.t.cpp` against `annotate list
///    --json`, whose renderer documents itself as returning NO terminator
///    and therefore needs one appended. The rule is per-renderer, read off
///    its `@return`; it is not a blanket policy.
///
/// 2. THE NON-ZERO EXIT PATH, cleanly. `workflow show nope` exits 1 with
///    `error: workflow 'nope' not found` on STDERR and nothing on stdout —
///    and the engine's `not_found_error` returns that WHOLE line, prefix
///    and terminator included, so it travels as a `rendered` domain_error
///    (see `planar.cmd.planar.exit`). Exit 1 is also the bucket most
///    easily tested vacuously, since it is the fallthrough: the tests pin
///    it alongside a 0 and a 2 from the same binary so a mapping that
///    collapsed everything to 1 could not pass.
///
/// 3. THE ENVIRONMENT SEAM. `catalog::resolve_dirs` takes an explicit
///    env-lookup callable, so this pair is what proves `context`'s `env`
///    member reaches an engine bucket intact — and, in the same stroke,
///    that a test can point the whole verb at a scratch `$PLANAR_HOME`
///    without any process-environment mutation.
///
/// Neither leaf touches SQLite, which is the point of the fourth thing
/// they prove: `context::db_opened()` is still false after either one
/// runs, so the lazy-database rule holds for real and not just in
/// principle.
module;

export module planar.cmd.planar.handlers.workflow;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar workflow list [--local] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success; this leaf has no failure path (an absent workflows
/// directory is an EMPTY catalog, never an error).
export auto workflow_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar workflow show <name> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) carrying the engine's
/// complete `error: workflow '<name>' not found` line when no workflow
/// matches.
export auto workflow_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar workflow run <name> --phase <phase> [--args <json>]
/// [--worktree <dir>] [--sandbox-root <dir>] [--local]`.
///
/// Port target: `zig/src/cmd/planar/handlers/workflow/run.zig` (167 lines).
/// The THIRD `workflow` leaf, deferred at task 6105 and wired at task 6272
/// once `planar.process` existed.
///
/// ## What this leaf is, exactly
///
/// `catalog::find` (already ported, and shared byte-for-byte with `workflow
/// show` — including `not_found_error`, which the two leaves emit
/// IDENTICALLY, single quotes included) plus a process spawn. There is no
/// engine half beyond what `show` already uses, which is what made this the
/// ONE leaf the process-spawn seam actually unblocked; see
/// `src/lib/process/CMakeLists.txt` for the other four that were predicted
/// and did not survive checking.
///
/// ## Three properties that are contract, not implementation detail
///
/// 1. **stdio is INHERITED, not captured.** The workflow's `flow.result`
///    JSON streams straight to the caller's terminal, and
///    `planar-execute`'s own diagnostics (`planar-execute: phase function
///    not found: build`) arrive on stderr in its OWN voice, not wrapped in
///    this binary's `error: ` prefix. Capturing and re-emitting would
///    change both.
/// 2. **The child's exit status is propagated EXACTLY**, via
///    `domain_error::passthrough_code` rather than a `domain_error_kind`
///    bucket — a workflow's `flow.fail` can end in a code no bucket names.
/// 3. **SQLite is never opened.** Resolution is filesystem-only, so
///    `ctx.db_opened()` stays false, exactly as it does for `list` and
///    `show`.
///
/// ## Binary resolution order, which is also the test seam
///
/// `$PLANAR_EXECUTE_BIN` -> sibling of `argv[0]` -> `planar-execute` on
/// `$PATH`. The first is read through `ctx.env()` (never `std::getenv` —
/// see `src/cmd/planar/CMakeLists.txt`), which is what lets the spawn be
/// tested against a stub script with no process-environment mutation and no
/// real `planar-execute` on the test machine.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success when the child exits 0; a `generic_failure` carrying the
/// child's exact status otherwise, or the engine's complete `error:
/// workflow '<name>' not found` line when no workflow matches.
export auto workflow_run(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
