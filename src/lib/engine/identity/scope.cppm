/// @file scope.cppm
/// @brief `planar.engine.identity.scope` — cwd-derived scope resolution
/// plus the cross-scope guard's refusal decision (tech-spec §
/// "engine buckets", plan 996 task cpp-scope-assoc).
///
/// Behavior-preserving port (D2) of:
///   - zig/src/engine/identity/scope.zig's `deriveFromCwd`, `resolveSlug`,
///     and `slugFromRef` — the cwd → scope derivation algorithm and the
///     `--scope \<slug\>` parsing/reverse-lookup plumbing.
///   - zig/src/engine/policy/scope_guard.zig's `check` — the pure
///     entity-scope-vs-write-scope comparison the cross-scope guard runs
///     before every guarded mutation (docs/concepts.md §cross-scope-guard).
///     Folded into this module rather than a separate `policy` bucket:
///     no `policy` module exists yet in the C++ tree, and the guard is one
///     small pure function tightly coupled to scope vocabulary — the Zig
///     comment on the guard's own file says "policy enforces rules ABOUT
///     scopes", and this port keeps that rule colocated with the scope
///     type it is about, subject to being split out if/when a `policy`
///     bucket lands.
///
/// `detectWorktree` — the Zig module's git-worktree-cwd classifier — is
/// NOT ported here. The task brief pointed at it as "already ported by
/// M1", but no such port exists anywhere under `src/` as of this task
/// (verified: `grep -ril worktree src/lib` before this change matched only
/// an unrelated CLI test fixture string). Worktree-aware cwd derivation is
/// therefore left for a future task; `derive_from_cwd` below always
/// resolves the literal `cwd` argument, with no worktree-to-parent-repo
/// redirection. This is a documented residual gap, not a silent omission
/// — see this task's coder report.
///
/// `resolve_for_write` models the CLI-side `--scope` override precedence
/// (explicit flag wins outright; otherwise fall back to cwd derivation)
/// as a pure engine-layer combinator so a future `cmd/` handler can call
/// one function instead of re-deriving the precedence rule. It is a NEW
/// composition point, not a line-for-line port: the equivalent logic in
/// the Zig tree lives split across `cmd/planar/scope.zig`'s much larger
/// `resolveForWrite` (workspace-root refusal, specificity ranking,
/// membership-aware candidate sets — all `cmd/`-layer concerns explicitly
/// out of scope for this task per the brief's CONSTRAINTS: "no cmd/ binary
/// yet"). This module's `resolve_for_write` covers only the
/// engine-layer-appropriate subset: explicit-flag-wins-over-cwd-derivation.
/// `guard_write`'s `no_scope_check` bypass parameter models the documented
/// `--no-scope-check` escape hatch (docs/concepts.md §cross-scope-guard,
/// "Escape hatch") as an engine-layer primitive; the current Zig binary
/// only wires that flag on one verb (`closure compute`) rather than
/// universally as the docs describe, but the bypass semantics themselves
/// (skip the check entirely) are unambiguous and reusable regardless of
/// which verbs end up passing `true`.
module;

export module planar.engine.identity.scope;

import std;
import planar.db;

namespace planar::engine::identity {

/// @brief Which kind of scope a `--scope <slug>` value (or a stored
/// entity's `scope_kind` column) names. Mirrors
/// zig/src/engine/identity/scope.zig's `ScopeKind`.
export enum class scope_kind : std::uint8_t {
  global,      ///< No project or association filter.
  association, ///< A named `associations` row.
  repo,        ///< A `projects` row, referenced as `repo:\<slug\>`.
};

/// @brief A resolved `(kind, id)` pair. `id` is unset for `scope_kind::global`.
/// Mirrors zig/src/engine/identity/scope.zig's `ScopeRef`.
export struct scope_ref {
  scope_kind                  kind; ///< Which kind of scope this ref names.
  std::optional<std::int64_t> id;   ///< The row id, unset for `scope_kind::global`.
};

/// @brief Why `derive_from_cwd` returned the scope it did. Mirrors
/// zig/src/engine/identity/scope.zig's `Reason`.
export enum class derive_reason : std::uint8_t {
  no_project_match,              ///< No project's root_path was a prefix of cwd.
  project_unassociated,          ///< Matching project has zero association memberships.
  project_single_association,    ///< Matching project has exactly one membership; that slug is used.
  project_multiple_associations, ///< Matching project has multiple memberships; ambiguous.
};

/// @brief Result of `derive_from_cwd`. Mirrors zig's `Resolution`, minus
/// the worktree-detection fields (see this file's header comment).
export struct scope_resolution {
  std::optional<std::string> scope;        ///< The derived association slug, or unset (global / ambiguous).
  derive_reason              reason;       ///< Why `scope` came out the way it did.
  std::optional<std::string> project_slug; ///< The matching project's slug, when a project matched at all.
};

/// @brief Result of `resolve_for_write`: the operator's write scope, plus
/// enough provenance for a caller to render a diagnostic.
export struct write_scope_resolution {
  std::optional<std::string> scope;                      ///< The resolved write-scope slug label, or unset (global).
  bool                       from_explicit_flag = false; ///< True when `scope` came from an explicit `--scope` value.
  derive_reason              reason = derive_reason::no_project_match; ///< Only meaningful when `from_explicit_flag` is false.
  /// @brief The matching project's slug, when cwd derivation matched a
  /// project at all; unset for an explicit `--scope` (no lookup happens)
  /// and for `derive_reason::no_project_match`.
  ///
  /// Carried because a caller that refuses on
  /// `derive_reason::project_unassociated` has to NAME the project in its
  /// remedy text — zig's `plan create` interpolates
  /// `resolution.project_slug` twice into the `planar assoc create
  /// project:<slug>` / `planar assoc add project:<slug>` instructions
  /// (zig/src/cmd/planar/handlers/plan/create.zig:28-36). Without it the
  /// caller would have to re-run `derive_from_cwd` purely to recover a
  /// value this resolution already computed. Mirrors zig's `Resolution`,
  /// which carries the same field for the same reason.
  std::optional<std::string> project_slug;
};

/// @brief Error surface for every fallible operation in this module.
export enum class scope_error : std::uint8_t {
  query_failed,   ///< An underlying SQL statement failed.
  invalid_path,   ///< `cwd` was empty or not absolute.
  slug_not_found, ///< A `--scope \<slug\>` value did not resolve to any row.
  scope_mismatch, ///< The cross-scope guard refused a write (see `check_scope_guard`).
};

/// @brief Derive the scope from `cwd` by walking the `projects` /
/// `associations` / `project_associations` tables.
///
/// Algorithm (mirrors zig's `deriveFromCwd`, minus worktree redirection —
/// see this file's header comment):
///   1. Find the project whose `root_path` is the longest prefix of `cwd`
///      (equal or `\<root_path\>/...`).
///   2. Load that project's association memberships, ordered by
///      `associations.id`.
///   3. Zero memberships -> `scope=unset`, `reason=project_unassociated`.
///      One membership -> that slug, `reason=project_single_association`.
///      2+ memberships -> `scope=unset`, `reason=project_multiple_associations`.
///   4. No project matched at all -> `scope=unset`, `reason=no_project_match`.
///
/// @param conn An open, migrated database connection.
/// @param cwd The absolute working-directory path to resolve from.
/// @return The resolution, or `scope_error::invalid_path` when `cwd` is
/// empty or relative, or `scope_error::query_failed` on a SQL failure.
export auto derive_from_cwd(db::connection& conn, std::string_view cwd) -> std::expected<scope_resolution, scope_error>;

/// @brief Resolve a `--scope` flag value to a `(kind, id)` pair.
///
/// Accepted forms (mirrors zig's `resolveSlug`):
///   "global"       -> {kind: global,      id: unset}
///   "\<slug\>"       -> {kind: association, id: \<assoc_id\>}
///   "assoc:\<slug\>" -> {kind: association, id: \<assoc_id\>}
///   "repo:\<slug\>"  -> {kind: repo,        id: \<project_id\>}
///
/// @param conn An open, migrated database connection.
/// @param slug The `--scope` flag's raw value.
/// @return The resolved ref, or `scope_error::slug_not_found` when the
/// association/project slug does not exist, or `scope_error::query_failed`
/// on a SQL failure.
export auto resolve_slug(db::connection& conn, std::string_view slug) -> std::expected<scope_ref, scope_error>;

/// @brief Reverse of `resolve_slug`: given a stored `(scope_kind,
/// scope_id)` pair, return the slug label the cross-scope guard compares
/// against. Mirrors zig's `slugFromRef`.
///
/// @param conn An open, migrated database connection.
/// @param kind The entity's stored scope kind.
/// @param id The entity's stored scope id. Ignored (may be unset) when
/// `kind == scope_kind::global`.
/// @return The slug label (unset for `scope_kind::global` — global is the
/// absence of a scope label), or `scope_error::slug_not_found` when
/// `kind != global` and no row with that id exists, or
/// `scope_error::query_failed` on a SQL failure.
export auto slug_from_ref(db::connection& conn, scope_kind kind, std::optional<std::int64_t> id)
    -> std::expected<std::optional<std::string>, scope_error>;

/// @brief The cross-scope guard's refusal decision (pure, no I/O). Mirrors
/// zig/src/engine/policy/scope_guard.zig's `check`. The matrix:
///
///   entity_scope unset -> entity is global; any write is allowed.
///   write_scope unset  -> resolver couldn't pin a scope; refuse to
///                         mutate any entity that has one.
///   else               -> must match exactly, after normalizing away an
///                         optional `assoc:` prefix on either side.
///
/// @param entity_scope The target entity's stored scope label (unset = global).
/// @param write_scope The operator's resolved write-scope label (unset = global/unresolved).
/// @return Success when the write is allowed, or `scope_error::scope_mismatch` when refused.
export auto check_scope_guard(std::optional<std::string_view> entity_scope, std::optional<std::string_view> write_scope)
    -> std::expected<void, scope_error>;

/// @brief `check_scope_guard`, with the documented `--no-scope-check`
/// escape hatch (docs/concepts.md §cross-scope-guard, "Escape hatch")
/// folded in: when `no_scope_check` is true, the check is skipped
/// entirely and the write is always allowed. Rendering the "downgrades to
/// a one-line stderr warning" half of that contract is a `cmd/`-layer
/// concern (this module has no output surface); callers that pass `true`
/// are expected to emit their own warning first.
///
/// @param entity_scope The target entity's stored scope label (unset = global).
/// @param write_scope The operator's resolved write-scope label (unset = global/unresolved).
/// @param no_scope_check When true, bypass the check unconditionally.
/// @return Success when the write is allowed (including every bypassed
/// case), or `scope_error::scope_mismatch` when refused.
export auto guard_write(std::optional<std::string_view> entity_scope, std::optional<std::string_view> write_scope,
                        bool no_scope_check) -> std::expected<void, scope_error>;

/// @brief Resolve the operator's write scope, applying the `--scope`
/// override precedence: an explicit flag always wins over cwd derivation
/// (never overridden — the flag is the user's stated intent).
///
/// An explicit `scope_flag` is threaded through VERBATIM, with no DB
/// lookup and no validation that it resolves to a real row — mirrors
/// zig's `resolveForWrite` (zig/src/cmd/planar/scope.zig:84-93), which
/// hands the override straight to the resolution with no call into
/// `resolveSlug`. Validating a `--scope` value against the DB is a
/// READ-path-only concern (`resolve_slug`, called from zig's
/// `resolveForReadSet`); an unknown write-scope slug surfaces later, as
/// an ordinary "no such scope" failure from whatever verb tries to use
/// it, not as an eager rejection here.
///
/// @param conn An open, migrated database connection.
/// @param scope_flag The `--scope` flag's raw value, when passed.
/// @param cwd The absolute working-directory path to fall back to deriving from.
/// @return The resolution, or `scope_error::invalid_path` when falling
/// back to `derive_from_cwd` on an invalid `cwd`, or
/// `scope_error::query_failed` on a SQL failure.
export auto resolve_for_write(db::connection& conn, std::optional<std::string_view> scope_flag, std::string_view cwd)
    -> std::expected<write_scope_resolution, scope_error>;

} // namespace planar::engine::identity
