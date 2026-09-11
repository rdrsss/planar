/// @file ext.cppm
/// @brief `planar.cmd.planar_ext.handlers.ext` — the four ported `planar ext`
/// leaves: `register jira`, `register github`, `list` (plan 996, task 6041)
/// and `test` (task 6258).
///
/// Port targets: zig/src/cmd/planar/handlers/ext/register/{jira,github}.zig,
/// ext/list.zig and ext/test.zig.
///
/// ## Why four of six
///
/// `ext test` was deferred through task 6041 on the adapter FACTORY — the
/// auth-resolution unit that turns an `external_systems` row into an adapter
/// instance. Task 6258 ported it as
/// `planar.cmd.planar_ext.handlers.ext_adapter_factory`, and `ext test` is the
/// leaf that makes it observable: SIX refusal messages (the header of that
/// module says why it is six and not the five this file previously claimed)
/// and one success line are its entire surface.
///
/// `ext create` / `ext propagate-one` / `ext propagate` remain deferred, and
/// the factory did NOT unblock them — they wait on the create/propagate half
/// of `engine_extsync` (see that bucket's CMakeLists.txt), which is a
/// separate absence. Neither deferral is speculative: each names the surface
/// it waits on.
///
/// ## The three renderers, and why they live HERE
///
/// `engine_external` has no render surface — the Zig originals emit these
/// bytes from the handler too — so the terminator belongs to this layer on
/// every path. All six payloads (three verbs x text/JSON) are captured
/// verbatim from the oracle and reproduced in `ext.t.cpp`'s header. The two
/// that are easy to get wrong:
///
///   - `ext list --json` is ONE JSON OBJECT PER LINE, not an array, and it
///     emits NOTHING AT ALL for an empty database (not `[]`, not a blank
///     line). The text form emits `no external systems registered\n`
///     instead, so the empty case is where the two shapes diverge most.
///   - `ext list --json` OMITS `base_url` and `default_project` entirely
///     when they are NULL rather than emitting `null`. Every currently
///     reachable registration sets both, so this only shows up on a row
///     written some other way — it is preserved because the Zig renderer
///     branches on the optional rather than serializing it.
///
/// ## A duplicate slug exits 6, not 1
///
/// `precondition_conflict`. Oracle-captured:
/// `error: external system 'gh-demo' already registered` on stderr with
/// exit 6. That is the same bucket `slug_conflict` maps to everywhere else
/// in this binary.
module;

export module planar.cmd.planar_ext.handlers.ext;

import std;
import cli11;
import planar.db;
import planar.cliapp.args;
import planar.engine.external;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;
import planar.cmd.planar_ext.handlers.ext_adapter_factory;

namespace planar::cmd::ext::handlers {

/// @brief Handle `planar ext register jira <slug> --base-url <u> --project
/// <p> --auth-env <e> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `slug_conflict` (exit 6) when the slug is taken.
export auto ext_register_jira(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar ext register github <slug> --project <p>
/// [--auth-env <e>] [--json]`.
///
/// Omitting `--auth-env` is not an error — it selects `gh-cli` auth.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `slug_conflict` (exit 6) when the slug is taken.
export auto ext_register_github(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar ext list [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) on a query failure.
export auto ext_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar ext test <slug> [--json]`.
///
/// Builds an adapter and reports whether it was wired. It does NOT contact
/// the remote — the oracle's own comment says so and its `probeOk` only
/// checks that construction produced an adapter — so this leaf reaches the
/// network on no path and is testable with no fixture server. The whole
/// observable surface is the six refusals in
/// `planar.cmd.planar_ext.handlers.ext_adapter_factory`, plus one success line.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, `not_found` (exit 1) for an unknown slug, or
/// `invalid_input` (exit 2) for any credential or kind refusal.
export auto ext_test(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar ext create <system-slug> --from <kind:id>
/// [--type <t>] [--role <r>] [--sync <d>] [--json]` (plan 996, task 6295).
///
/// Renders the local entity through the adapter, POSTs the payload, and
/// records the resulting `external_links` row.
///
/// ## IT NEEDS NONE OF `engine_extsync`'s UNPORTED LINES
///
/// The prediction carried into this cycle was that `ext create` waited on
/// the create/propagate half of `engine_extsync` (~3665 Zig lines). It does
/// not. Its only uses of that surface are `common.{LocalEntity,
/// CreateOptions, Header}`, all three of which already existed here as
/// `adapter::local_entity`, `adapter::create_options` and `http::header`.
/// What was actually missing was two `adapter_handle` accessors — see
/// `ext_adapter_factory.cppm` for why the creation path cannot go through
/// the `external_adapter` interface.
///
/// ## ITS ADAPTER-BUILD REFUSALS ARE PROSE AT EXIT 2, LIKE `ext test`
///
/// AND NOT like the `sync` trio, which emits the raw Zig tag at exit 1.
/// That rule was learned on the sync verbs and does NOT generalise:
/// `ext/create.zig` maps every factory error to `error.InvalidInput` with
/// an interpolated message, so `factory_error_message` is the right
/// reference here and `factory_error_name` (`handlers/sync.cpp`) is the
/// wrong one. Verified against the running oracle, not inferred from either
/// sibling.
///
/// ## `--from` IS PARSED LOOSELY AND VALIDATED LATE, AND THAT IS OBSERVABLE
///
/// `handlers/sync.cppm`'s `parse_kind_id_ref` is deliberately NOT reused
/// here despite being the obvious candidate. It validates the kind against
/// the seven `external_entity_kind` spellings; the oracle's `ext create`
/// parses ANY non-empty kind and lets the local read refuse. The two
/// disagree on real input:
///
///   --from foo:1        parses here, and refuses with
///                       `ext create: read local foo:1: InvalidInput`.
///                       `parse_kind_id_ref` would have refused earlier
///                       with `invalid --from value` — a different message.
///   --from decision:1   is a VALID `external_entity_kind` and still
///                       refuses, because the local read serves only
///                       task / plan / question / artifact. So the two kind
///                       sets are genuinely different sizes: seven that a
///                       link may point at, four this verb can read.
///
/// ## NOTHING REACHES THE REMOTE UNTIL EVERY REFUSAL HAS FIRED
///
/// The order is: `--from` shape, system lookup, adapter build, local read,
/// `--role` / `--sync`, entity kind, THE DUPLICATE GATE, render, POST,
/// insert. Every step that can refuse precedes the POST.
///
/// It did not always. Under D2 this verb reproduced the oracle's ordering:
/// `--role bogus` POSTed the ticket and THEN refused at exit 2 (task 6312),
/// and a repeat POSTed a SECOND ticket before discovering the existing link
/// and refusing at exit 6 (task 6313). Decision 1067 retired D2's
/// bug-for-bug rule together with the oracle it existed to serve, and put
/// both rows in its FIX half: alone among the nine reproduced divergences,
/// this one's consequence LEAVES THE COMMAND. It writes a ticket into a
/// system Planar does not own, cannot roll back, and holds no record of — a
/// typo in a flag left an issue for somebody to find and close by hand.
///
/// The fix is a pure reordering plus one added read; no message, exit code
/// or JSON shape moved. `ext_create_leaf.t.cpp`'s ordering case now pins the
/// request count at ZERO for every refusal, having previously pinned it at
/// one.
///
/// ## THE DUPLICATE GATE IS NOT THE CONSTRAINT, AND IT IS ROLE-SCOPED
///
/// It is a `link::list` on `(entity_kind, entity_id, system_id)` whose rows
/// are then narrowed to the requested `link_role` — a four-field key. The
/// refusal names the role: `mirror external link for task:1 on jira-demo
/// already exists`.
///
/// The role belongs in the key because REPEATING A COMMAND is what 6313 is
/// about, and a repeat carries the same `--role`. Dropping it would be
/// adopting a new cardinality rule — one link per entity per system — that
/// nothing else in this tree enforces. `load_existing_mirror`'s contract
/// says in as many words that a `parent` or `child` row for the same entity
/// does not suppress propagation, and `planar link <kind:id> --to
/// <slug>:<id>` defaults `--role` to `reference` and gates on nothing but
/// the UNIQUE. A wide key here would refuse `--role reference` on an entity
/// that already had a mirror while `planar link` still allowed exactly that
/// row: one table, two verbs, two cardinality rules, and no recovery from
/// the CLI. `docs/cli-reference.md` advertises all four roles on this verb
/// with no stated restriction. Narrowing that is a product decision, and
/// decision 1067's FIX set does not contain one.
///
/// The gate is NOT `load_existing_mirror` either: that hard-filters
/// `link_role = 'mirror'`, which is right where it lives (`propagate-one`
/// only ever writes mirrors) but wrong here, where the role comes from a
/// flag.
///
/// And the table's `unique (entity_kind, entity_id, system_id,
/// external_id)` is not a substitute, for a reason easy to miss from a
/// fixture: a real Jira or GitHub mints a FRESH id on every POST, so a
/// second create's insert would have SUCCEEDED, leaving two tickets and two
/// links with no refusal at all. The exit-6 refusal 6313 recorded was an
/// artifact of a fixture server that answers with a fixed id. The
/// post-insert `link_exists` branch survives as a backstop for a concurrent
/// create only.
///
/// ## ONE ORPHAN PATH REMAINS, AND IT IS THE IRREDUCIBLE ONE
///
/// If the POST succeeds and `link::create` then fails, the remote issue
/// exists with no local row. Three ways in: a genuine SQLite error; a
/// concurrent create winning the race between the gate's read and the
/// insert; or a remote that answers with an `external_id` it has already
/// handed this entity+system under a DIFFERENT role, which the gate lets
/// past and the table's UNIQUE then rejects. The third is why the
/// `link_exists` branch after the POST is not dead code — a live Jira or
/// GitHub mints a fresh key per POST, but no adapter contract promises it.
///
/// Closing any of them needs a remote rollback this verb has no adapter
/// operation for. The reordering removes the two orphan paths that were
/// reachable from a typo and a repeat; it introduces none.
///
/// The refusal after the POST keeps the ROLE-LESS wording (`external link
/// for task:1 on jira-demo already exists`) while the gate before it names
/// the role. That is deliberate: the two refusals key on different columns,
/// and naming the requested role in the constraint case would misreport
/// which row actually collided.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success; `not_found` (exit 1) for an unknown slug or an absent
/// local row; `invalid_input` (exit 2) for a malformed ref, an unreadable
/// kind, a credential refusal or a bad `--role` / `--sync`; or
/// `slug_conflict` (exit 6) when the link already exists.
export auto ext_create(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar ext propagate-one <system> --from <kind:id> [--strategy s]
/// [--sync d] [--dry-run] [--json]`.
///
/// Render one entity's template, POST the counterpart, record the
/// `external_links` row.
///
/// ## It needed 36 of `propagate.zig`'s 409 lines
///
/// Carried as blocked on the whole create/propagate half of
/// `engine_extsync` (3665 lines). Measured by symbol at task 6335 it reaches
/// exactly two functions — `strategyForSystem` and `loadExistingMirror` —
/// and neither reaches anything else in that surface. They landed as
/// `engine::extsync::propagate::strategy_for_system` (pure) and
/// `engine::external::link::load_existing_mirror` (SQL), split across two
/// buckets because `engine_extsync` carries no `db` edge. `parent_issue.zig`
/// and `projects_v2.zig` — 2394 lines the brief flagged as possibly
/// unnecessary — are reached by NOTHING here.
///
/// ## THIS is the idempotent one, and it is idempotent for a structural reason
///
/// `load_existing_mirror` is the FIRST thing this verb does, before the
/// template is even loaded, and every argument refusal (`--from` shape,
/// entity kind, `--strategy`, `--sync`) precedes both the adapter build and
/// the POST. A repeat returns `op:"skipped"` carrying the EXISTING external
/// id and sends nothing.
///
/// This is the shape `ext create` was made to match under decision 1067,
/// where it had reproduced the oracle's side-effect-first ordering (tasks
/// 6312 / 6313). The two gates are still not the same query — see
/// `ext_create`'s header on why this one cannot be reused there.
///
/// The one refusal here that DOES follow the POST,
/// `external_entity_kind_from_text` on the way to the insert, is unreachable
/// from the CLI: `ext_propagate_one` admits only `plan` and `task`, and both
/// are valid kinds. It is latent, not operator-reachable, which is why it
/// was left alone rather than folded into those rows.
///
/// Note `workbench publish` is a THIRD shape again — it REFUSES on an
/// existing link rather than skipping.
///
/// ## `--strategy` accepts one value and names two others to refuse them
///
/// `parent-issue` and `projects-v2` are recognized only so they can be
/// rejected with the advice to use `ext propagate --github-strategy`: both
/// need the feature-tree walk this per-entity primitive does not do.
/// `tracking-issue` is the only accepted value. Anything else is a generic
/// invalid-value refusal. All three arms are exit 2.
///
/// ## The strategy affects the OUTPUT, not the template
///
/// `template_kind_for_entity` discards `strategy_kind` for GitHub outright —
/// all GitHub strategies share the same three template kinds. The resolved
/// strategy reaches the emitted JSON `"strategy"` field and (for an anchor)
/// the link's `config_json` cache, and nothing else. A reader who assumes
/// `--strategy` selects a template will misread this verb.
///
/// ## `--dry-run` builds NO adapter
///
/// It renders the payload and returns `op:"planned"` with a placeholder
/// `<template-kind>` id. Because the adapter is never built, a dry run does
/// not resolve credentials and cannot fail on a missing token.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto ext_propagate_one(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief How an entity sits in the feature tree, which picks its template.
/// Exported (task 6451) so `planar.cmd.planar_ext.handlers.propagate`'s
/// generic per-entity tree-walk loop can share it with `ext_propagate_one`
/// rather than re-deriving the same three-way split from
/// `planar.engine.planning.descendants::entry_kind`.
export enum class entity_role : std::uint8_t {
  plan_anchor, ///< The feature's root plan.
  plan_child,  ///< A plan with a parent.
  task,        ///< A task.
};

/// @brief Map `(system_kind, role)` to the template kind to render.
///
/// `strategy_kind` is deliberately NOT a parameter: the oracle takes it and
/// discards it for GitHub (`_ = strategy_kind;`), and Jira never branches on
/// it either.
/// @param system_kind `"jira"` or `"github-issues"`.
/// @param role How the entity sits in the feature tree.
/// @return The template kind, or `std::nullopt` for an unrecognized system kind.
export auto template_kind_for_entity(std::string_view system_kind, entity_role role) -> std::optional<std::string_view>;

/// @brief The outcome of `propagate_one_entity`: what happened, and the
/// resulting (or pre-existing) provider id.
export struct propagate_one_outcome {
  std::string op;          ///< One of `"skipped"`, `"planned"`, `"created"`.
  std::string external_id; ///< The provider id — existing, placeholder (`"<template-kind>"` under dry-run), or freshly created.
};

/// @brief Resolve the operator's templates directory:
/// `$PLANAR_TEMPLATES_DIR` > `[templates] dir` in the config file > the
/// embedded default, tilde expanded.
///
/// Exported (task 6451) so `planar.cmd.planar_ext.handlers.propagate`'s
/// generic per-entity tree-walk loop resolves the SAME root
/// `ext_propagate_one` does, rather than re-deriving config/env resolution
/// a second time.
/// @param ctx The invocation context.
/// @return The resolved root, or the exit-1 refusal.
export auto templates_root_for(context& ctx) -> std::expected<std::string, domain_error>;

/// @brief The shared per-entity propagation body: idempotency check, render,
/// POST, record `external_links` — ported from the oracle's
/// `propagateOneEntity` (`propagate_one.zig`), which BOTH `ext propagate-one`
/// and the generic arm of `ext propagate`'s loop call, guaranteeing the two
/// verbs execute identical logic rather than merely similar logic (task
/// 6451; this is the equivalence `propagate_faithful_test.zig` pins).
///
/// `handle` is `nullptr` under `dry_run` — this function never dereferences
/// it on that path, mirroring `ext_propagate_one`'s own "dry-run builds no
/// adapter" invariant.
/// @param conn An open, migrated database connection.
/// @param handle The built adapter handle, or `nullptr` under `dry_run`.
/// @param sys The target external system.
/// @param entity_kind `"plan"` or `"task"`.
/// @param entity_id The local row id.
/// @param role How the entity sits in the feature tree (only the anchor caches the strategy).
/// @param strategy_kind The resolved strategy name, cached into the anchor's `config_json` on first link.
/// @param template_kind The template kind to render, from `template_kind_for_entity`.
/// @param direction The sync direction recorded on a newly created link.
/// @param dry_run When true, no adapter is contacted and no row is written.
/// @param templates_root The resolved templates root.
/// @return The outcome, or the failure.
export auto propagate_one_entity(db::connection& conn, adapter_handle* handle,
                                 const engine::external::system::external_system& sys, std::string_view entity_kind,
                                 std::int64_t entity_id, entity_role role, std::string_view strategy_kind,
                                 std::string_view template_kind, engine::external::link::sync_direction direction, bool dry_run,
                                 std::string_view templates_root) -> std::expected<propagate_one_outcome, domain_error>;

} // namespace planar::cmd::ext::handlers
