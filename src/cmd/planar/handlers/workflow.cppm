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

} // namespace planar::cmd::handlers
