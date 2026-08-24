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

} // namespace planar::cmd::handlers
