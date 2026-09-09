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

/// @brief Handle `planar assoc members <slug> [--json]`.
///
/// Port target: zig/src/cmd/planar/handlers/association/members.zig (plan
/// 996, task 6188).
///
/// ## Why this leaf lagged the rest of its family
///
/// `engine_identity` has exported `members()` since the family was first
/// ported; what it lacked was a renderer. `association.cppm` exported only
/// `render_text(const association&)` and `render_json(const
/// association&)`, both SINGULAR and both over the wrong type — `members()`
/// returns `std::vector<project_ref>`. Task 6188 added
/// `render_member_list_text` / `render_member_list_json` alongside them.
///
/// The leaf is worth more than one verb: five
/// `groups_recommend_test` integration frames were still crashing after
/// `groups recommend` itself was wired, because they die in the FIXTURE at
/// `assoc members --json` rather than in the verb under test.
///
/// An unknown association REFUSES with `no association named '<slug>'` at
/// exit 1 — the same wording `assoc add` uses for the same condition, and
/// oracle-captured on both. Listing empty instead would be
/// indistinguishable from a real association with no members, which is a
/// legitimate state that prints `(no members)`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) for an unknown
/// association or any other engine failure.
export auto assoc_members(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar assoc list [--kind] [--json]`.
///
/// Port target: zig/src/cmd/planar/handlers/association/list.zig (plan
/// 996, task 6279).
///
/// ## `--kind` refuses at exit 2, and an ABSENT `--kind` cannot refuse
///
/// The same split `assoc_create`'s header documents, and for the same
/// reason: zig only calls `Kind.fromText` inside `if (args.kind) |k|`, so
/// an absent flag leaves `ListFilter.kind` null and never reaches the
/// refusal. The message is byte-identical to `assoc create`'s — `unknown
/// kind '<value>'` — and so is its `error.InvalidInput` -> exit 2 mapping.
///
/// `--kind ''` is NOT absent. An empty string is a present flag whose
/// value parses as no kind, so it REFUSES at exit 2 rather than listing
/// everything. That distinction is the one an implementer is most likely
/// to erase by testing `flag_string(...).value_or("")` for emptiness.
///
/// ## This leaf is NOT scope-resolved
///
/// Same reason `assoc create` is not: an association IS a scope. `list`
/// answers over every row in the table regardless of where it is run from,
/// and the zig handler correspondingly never touches `scope.zig`. There is
/// no cwd-derived filter to apply and adding one would silently hide rows.
///
/// An empty table is a LISTING (`(no associations)` / `[]`), not a
/// refusal — the opposite posture from `assoc members` on an unknown slug,
/// because "no associations exist" is a real answer where "no association
/// named X" is a bad argument.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for an unknown `--kind`,
/// or `generic_failure` (exit 1) on any engine failure.
export auto assoc_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar assoc remove <slug> <repo-path> [--json]`.
///
/// Port target: zig/src/cmd/planar/handlers/association/remove.zig (plan
/// 996, task 6279).
///
/// ## The two bespoke messages are NOT `assoc add`'s two
///
/// `assoc add` spells its pair `no association named '<slug>'` and
/// `project at '<path>' is already a member of '<slug>'`. This verb shares
/// only the FIRST. Its second arm is `NotAMember`, and the oracle words it
/// `no project registered at '<repo-path>'` — a sentence about the PROJECT
/// that names neither the association nor the membership, because
/// `removeMember` reaches `NotAMember` both when no `projects` row is
/// registered at the path at all and when one is but is not linked. Both
/// exit 1.
///
/// ## The path is matched VERBATIM, and that is where task 6256 bites
///
/// `remove_member` looks the project up by `root_path` string equality
/// against whatever `assoc add` stored. `assoc add` stores its argument
/// uncanonicalised, so `assoc add acme .` writes the literal `.` and this
/// verb can only remove it by being handed the literal `.` back. A fixture
/// that seeds with `.` and removes with an absolute path gets
/// `NotAMember` — and a test asserting only "the membership is gone"
/// passes against a membership that was never created. Seed with absolute
/// paths and assert the row EXISTS before removing it.
///
/// ## Success output is hand-rolled and the JSON is deliberately unescaped
///
/// Same shape and same reason as `assoc_add`'s: `remove_member` returns
/// `void`, so the oracle prints a fixed sentence built from the two
/// ARGUMENTS with a raw `{s}` substitution and no `writeJsonString` call.
/// A slug or path containing a double quote therefore produces invalid
/// JSON on both sides. Escaping here would be better JSON and a byte-level
/// divergence.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) for an unknown
/// association, an unregistered/unlinked project, or any other engine
/// failure.
export auto assoc_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar assoc detect [--apply] [--json]` — propose (or create)
/// auto-detected associations for the operator's cwd. Plan 996, task 6325.
///
/// ## Without `--apply` this verb writes NOTHING
///
/// The default is a preview: `detect_proposals` probes the filesystem and
/// `enrich_proposals` reads the DB, but no row is written. A test asserting
/// only that the command exits 0 cannot tell the preview from the mutation
/// — it must assert `assoc list` is UNCHANGED afterwards.
///
/// ## It opens the database even in preview mode
///
/// `ensure_db` runs first, so an invocation in a directory with no database
/// still creates and migrates one before printing a proposal. Same ordering
/// (and same reason) as `assoc_create`'s.
///
/// ## `--apply` re-enriches, so its output is not the pre-apply output
///
/// After applying, the oracle calls `enrich_proposals` a SECOND time and
/// discards any failure. Every proposal therefore prints `already a member`
/// on a successful `--apply`, never the `will create` the same invocation
/// would have shown a moment earlier. A test that expects `--apply` to echo
/// the preview's labels is asserting the wrong contract.
///
/// ## The refusal names `planar init`, and it fires before the empty check
///
/// `--apply` in an unregistered directory exits 1 with `no project
/// registered at cwd (<path>); run `planar init` first` — including when
/// there were no proposals to apply at all, because the engine resolves the
/// project before it loops. Only `--apply` can fail this way; the preview
/// path tolerates an unregistered cwd.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) when `--apply` finds no
/// registered project at the cwd or any engine call fails.
export auto assoc_detect(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `assoc` command tree on `root`.
///
/// The CLI declaration for every `assoc` node, colocated with the handlers
/// above (plan 1051, M11.3d — decision 1068). No `assoc` node was ever
/// hand-declared in `tree.cpp`, so all seven came from `surface.cpp`'s
/// generated table and nothing here had to be reconciled against a
/// shadowing hand declaration.
///
/// The group answers to `association` as well as `assoc`, but that alias
/// is resolved ABOVE the tree — see `planar.cmd.planar.dispatch` — so it
/// is deliberately not declared here.
/// @param root The root app to attach the `assoc` group to.
export auto declare_assoc(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
