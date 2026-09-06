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
/// ## THE REMOTE IS CREATED BEFORE `--role` AND `--sync` ARE VALIDATED
///
/// Oracle-captured from the fixture server's own request log, and preserved
/// under D2: `ext create <sys> --from task:1 --role bogus` POSTs the
/// ticket, THEN refuses at exit 2, leaving a real remote issue with no local
/// link. A duplicate `ext create` does the same — it POSTs a SECOND ticket
/// before discovering the existing link and refusing at exit 6. Both are
/// defects in the oracle rather than in this port, and both are pinned in
/// `ext_create_leaf.t.cpp` so that fixing them is a deliberate, recorded
/// divergence rather than a silent one.
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
/// `ext create` has two recorded side-effect-first defects: it POSTs before
/// validating `--role` (6312) and POSTs a SECOND ticket on a repeat (6313).
/// This verb has neither, and not by accident — `load_existing_mirror` is the
/// FIRST thing it does, before the template is even loaded, and every
/// argument refusal (`--from` shape, entity kind, `--strategy`, `--sync`)
/// precedes both the adapter build and the POST. A repeat returns
/// `op:"skipped"` carrying the EXISTING external id and sends nothing.
///
/// This shape is the one to copy when 6312/6313 are eventually fixed. Note
/// `workbench publish` is a THIRD shape again — it REFUSES on an existing
/// link rather than skipping.
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

export auto propagate_one_entity(db::connection& conn, adapter_handle* handle,
                                 const engine::external::system::external_system& sys, std::string_view entity_kind,
                                 std::int64_t entity_id, entity_role role, std::string_view strategy_kind,
                                 std::string_view template_kind, engine::external::link::sync_direction direction, bool dry_run,
                                 std::string_view templates_root) -> std::expected<propagate_one_outcome, domain_error>;

} // namespace planar::cmd::ext::handlers
