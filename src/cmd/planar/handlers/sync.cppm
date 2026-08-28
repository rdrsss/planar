/// @file sync.cppm
/// @brief `planar.cmd.planar.handlers.sync` — the `sync pull`, `sync push`
/// and `sync resolve` leaves (plan 996, task 6294).
///
/// Port target: `zig/src/cmd/planar/handlers/sync/{pull,push,resolve,
/// common}.zig` (166 + 158 + 105 + 131 lines).
///
/// ## THESE THREE LEAVES NEED NOTHING FROM `engine_extsync`
///
/// Task 6294's brief predicted that these three and the three `ext`
/// create/propagate leaves all pointed at "the create/propagate half of
/// `engine_extsync`" (~3665 unported Zig lines across `common.zig`,
/// `propagate.zig`, `strategy.zig`, `parent_issue.zig`, `projects_v2.zig`).
/// For THESE three that is false, and checkably so: none of
/// `zig/src/cmd/planar/handlers/sync/{pull,push,resolve,common}.zig`
/// contains the token `extsync` at all. What they call is
/// `engine.external.sync.{pullLink, pushLink, resolveConflict}` —
/// `zig/src/engine/external/sync.zig`, a DIFFERENT module that was already
/// ported in full as `planar.engine.external.sync` (766 lines of
/// implementation and a 980-line test file). The similarity of the two
/// module names is the whole trap; see the task report.
///
/// So what landed here is handler wiring over an engine that was already
/// finished, plus the one genuinely-missing cmd-layer helper below.
///
/// ## THE ORACLE'S `Handle`-KIND SWITCH DOES NOT SURVIVE THE PORT
///
/// `sync/common.zig` carries three near-identical functions (`pullLink`,
/// `pushLink`, `resolveConflict`) whose entire body is
/// `switch (handle.kind) { .jira => …jira_adapter.?, .github =>
/// …github_adapter.? }`. That switch exists because the Zig engine takes its
/// adapter as `anytype` — a comptime duck type, so the two concrete adapters
/// are unrelated types and the caller must pick the field.
///
/// The C++ engine takes `const adapter::external_adapter&`, a virtual
/// interface, and `adapter_handle::instance()` returns exactly that. The
/// three wrappers therefore have nothing left to do and are deliberately NOT
/// reproduced: reproducing them would mean re-introducing a
/// `switch (handle.kind())` whose two arms call the same function with the
/// same argument. `adapter_kind` stays on the handle because `ext` verbs
/// that build a provider-specific URL still need it.
///
/// ## `guard_with_membership` IS THE ONE HELPER THAT HAD TO BE PORTED
///
/// `engine::identity::check_scope_guard` is the pure comparison and was
/// already ported. The membership-aware wrapper
/// (`zig/src/cmd/planar/scope.zig:569`) was not, and it is not decorative:
/// it is what lets an operator working at an ASSOCIATION scope sync a link
/// whose entity is stored at a MEMBER REPO scope. Without it every such
/// pull/push refuses at exit 5. The asymmetry is deliberate in the oracle
/// and preserved here — an association write scope covers its member repos,
/// a repo write scope never covers the association.
///
/// It lives in this module rather than in `planar.cmd.planar.scope` because
/// these three leaves are its only callers today. When a second family needs
/// it, it moves; a shared helper with one caller is a guess about the
/// future, and D19 is about deduplicating drift that EXISTS.
///
/// ## THE TWO EXIT CODES `pull` HAS AND `push` DOES NOT
///
/// `sync pull` ends in one of three states and they are NOT collapsible:
///
///   - an adapter failure on any link  -> exit 1  ("one or more pull errors")
///   - no failure but a CONFLICT seen  -> exit 3  (sync-conflict bucket)
///   - neither                         -> exit 0
///
/// and the conflict arm is guarded on `!had_error`, so a run with both a
/// failure and a conflict reports the FAILURE. `sync push` has no conflict
/// arm whatsoever — the engine's `push_link` is documented as unconditional
/// and never returns `outcome::conflict` — so push is exit 1 or exit 0 only.
/// A port that gave push a conflict arm "for symmetry" would be adding a
/// branch no input can reach.
///
/// Both verbs keep going after a per-link failure and report at the END:
/// the per-link error line is written to STDOUT (not stderr) as part of the
/// result stream, and only the summary refusal goes to stderr. That is why
/// `--json` still emits a well-formed result object for a failed link, with
/// `"outcome":"error"` and the error name in `detail`.
///
/// ## THE SCOPE GUARD RUNS ONLY WHEN `--all` WAS NOT PASSED
///
/// Oracle-preserved and counter-intuitive: `--all` skips the cross-scope
/// guard entirely. `--all` means "every pullable link in the database",
/// which is inherently cross-scope, so guarding it would make the flag
/// unusable rather than safe. The targeted form guards every resolved link.
module;

export module planar.cmd.planar.handlers.sync;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.external;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief A parsed `<kind>:<id>` external-entity reference, e.g. `task:42`.
///
/// Uses the narrower `external_entity_kind` (the kinds an external link may
/// point at), NOT the wider entity-link kind set — so `plan_step` and `repo`
/// are rejected here by design.
export struct kind_id_ref {
  engine::external::link::external_entity_kind kind{}; ///< Which local table.
  std::int64_t                                 id = 0; ///< The local row id.
};

/// @brief Parse a `<kind>:<id>` reference.
///
/// Splits on the LAST colon, not the first, mirroring the oracle's backward
/// scan. An empty kind, an empty id, an unknown kind or a non-integer id all
/// fail the same way.
///
/// The scan DIRECTION is currently unobservable and is kept only for
/// fidelity: a break-probe that reversed it survived the whole suite, and a
/// four-case oracle differential over `task:1:2`, `:task:5`, `task:5:` and
/// `TASK:5` confirmed why — no `external_entity_kind` spelling contains a
/// colon, so both directions reject exactly the same inputs. If a
/// colon-bearing kind is ever added, the direction becomes load-bearing and
/// those cases in `sync_leaves.t.cpp` start discriminating.
/// @param text The raw reference.
/// @return The parsed reference, or unset when malformed.
export auto parse_kind_id_ref(std::string_view text) -> std::optional<kind_id_ref>;

/// @brief The scope label the cross-scope guard compares a link's entity
/// against, or unset when the entity is inherently unscoped.
///
/// `session` links return unset unconditionally: sessions carry no
/// `(scope_kind, scope_id)` pair, so there is nothing to compare and the
/// guard treats them as global. Every other kind reads the pair from its own
/// table and resolves it through `slug_from_ref`.
/// @param conn An open, migrated database connection.
/// @param row The link whose entity is being guarded.
/// @return The label (unset for global / session), or the failure.
export auto entity_scope_slug(db::connection& conn, const engine::external::link::ext_link& row)
    -> std::expected<std::optional<std::string>, domain_error>;

/// @brief The membership-aware cross-scope guard.
///
/// Delegates to `engine::identity::check_scope_guard` first. On a refusal it
/// applies exactly one widening: an entity at `repo:<project>` is allowed
/// when the operator's write scope is an ASSOCIATION that the project is a
/// member of. Every other refusal stands, and the reverse direction (a repo
/// write scope reaching an association entity) is never widened.
/// @param conn An open, migrated database connection.
/// @param entity_scope The entity's scope label (unset = global).
/// @param write_scope The operator's resolved write scope (unset = global).
/// @return Success when the write is allowed, or `false` when refused.
export auto guard_with_membership(db::connection& conn, std::optional<std::string_view> entity_scope,
                                  std::optional<std::string_view> write_scope) -> bool;

/// @brief `planar sync pull <ref> | --all [--system <slug>] [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto sync_pull(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar sync push <ref> | --all [--system <slug>] [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto sync_push(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar sync resolve <event-id> --keep <local|remote>
/// --evidence-token <t> --expected-local-updated-at <v> [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto sync_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar sync status [--entity <kind:id>] [--system <slug>] [--json]`.
///
/// ## IT SHARES NOTHING WITH THE OTHER THREE BUT `parse_kind_id_ref`
///
/// The inventory note this leaf left behind called it out as "NOT a fourth
/// free leaf", and the shape is why: it renders `engine::external::sync::
/// status` rows — a LISTING none of pull/push/resolve produces — it takes
/// `--entity` rather than a positional ref, and it runs NO cross-scope
/// guard at all. `guard_with_membership` is not reached from here. Adding
/// the guard "for consistency with its siblings" would refuse reads the
/// oracle allows.
///
/// ## `--entity` IS A FILTER, AND A MALFORMED ONE REFUSES BEFORE THE QUERY
///
/// `--system` is passed straight through as `list_filter::system_slug` with
/// no existence check, so an unknown slug matches nothing and exits 0.
/// `--entity` is different: it is PARSED, and a malformed value refuses at
/// exit 2 without touching the database. The asymmetry is the oracle's --
/// an unknown system is an empty result, an unparseable entity is an error.
///
/// ## THE JSON FORM IS LINE-DELIMITED, AND EMPTY MEANS EMPTY
///
/// Each row is its own object on its own line; there is no enclosing array
/// and no separating comma. An empty match set therefore emits ZERO BYTES
/// under `--json` -- not `[]` -- while the text form prints
/// `no external links`. Both exit 0. A port that emitted `[]` would be
/// well-formed JSON and still wrong.
///
/// `last_synced_at` is OMITTED from the object when NULL rather than
/// emitted as `null`, so the key set varies row to row.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto sync_status(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
