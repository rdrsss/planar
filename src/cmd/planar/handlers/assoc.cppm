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

/// @brief Handle `planar assoc add <slug> <repo-path> [--json]`.
///
/// Port target: zig/src/cmd/planar/handlers/association/add.zig (plan 996,
/// task 6135).
///
/// ## The path is taken VERBATIM and is not validated
///
/// `repo-path` is the `projects.root_path` key the cwd-derive resolver
/// later matches against, so any normalisation applied here is a
/// normalisation the resolver does not apply — and the two keys stop
/// matching. The oracle does not canonicalise, does not require the
/// directory to exist, and does not make the path absolute: `planar assoc
/// add acme /nonexistent/path/xyz` exits 0 and registers a `projects` row
/// with `root_path='/nonexistent/path/xyz'` and `slug='xyz'`
/// (oracle-captured). A `std::filesystem::canonical` or `absolute` call
/// here would look like an improvement and would silently break the
/// `/var` -> `/private/var` case `context::operator_cwd`'s header already
/// documents at length.
///
/// ## Two of the three failures get BESPOKE messages
///
/// `NotFound` and `AlreadyMember` are the operator-reachable ones and the
/// oracle spells each out in full rather than printing an error tag;
/// everything else falls through to the `association add: <ErrorName>`
/// shape `assoc_create` also uses. All three exit 1 — including
/// `AlreadyMember`, which is a duplicate-membership collision but is NOT
/// mapped to the slug-conflict bucket (6): `codeFor` maps
/// `error.SlugConflict`/`error.AlreadyExists`, and `error.AlreadyMember`
/// is neither. Oracle-confirmed on both.
///
/// ## Success output is hand-rolled, not a renderer
///
/// `add_member` returns `void` — there is no row to render — so the
/// oracle prints a fixed sentence (text) or a four-field literal (JSON)
/// built from the two ARGUMENTS, not from anything read back out of the
/// database. Reproduced the same way, including the `"status":"added"`
/// field that exists nowhere in the schema.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) for an unknown
/// association, an already-registered membership, or any other engine
/// failure.
export auto assoc_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
