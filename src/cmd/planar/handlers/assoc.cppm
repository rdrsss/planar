/// @file assoc.cppm
/// @brief `planar.cmd.planar.handlers.assoc` — the `planar assoc create`
/// leaf (plan 996, task 6133).
///
/// Port target: zig/src/cmd/planar/handlers/association/create.zig.
///
/// ## Deliberately NOT scope-resolved
///
/// The sibling `plan create` in this same task cwd-derives its write
/// scope and refuses when the derivation comes back unassociated. This
/// verb does neither, and the difference is not an oversight: an
/// association IS a scope. There is nothing above it to file it under,
/// `associations` has no scope columns, and the zig handler correspondingly
/// never touches `scope.zig`. Adding a resolution here would invent a
/// concept the row cannot store.
///
/// ## `--kind` refuses at exit 2, `plan create`'s `--status` at exit 1
///
/// Both are "operator typed a bad enum value", and the two verbs
/// disagree. zig dies here with `error.InvalidInput` (mapped to 2 in
/// `exit.zig`'s `codeFor`) and there with `error.InvalidStatus` (no arm,
/// so 1). Oracle-captured on both sides rather than inferred; do not
/// "fix" the inconsistency.
///
/// An unset `--kind` is NOT the same as `--kind ad-hoc` at the handler
/// level: zig leaves `CreateArgs.kind` at its default when the flag is
/// absent and only parses when it is present, so an absent flag can never
/// produce the unknown-kind refusal. The default lands in the engine.
module;

export module planar.cmd.planar.handlers.assoc;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar assoc create <slug> [--name] [--kind] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for an unknown `--kind`,
/// `slug_conflict` (exit 6) when the slug already exists, or
/// `generic_failure` (exit 1) on any other engine failure.
export auto assoc_create(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
