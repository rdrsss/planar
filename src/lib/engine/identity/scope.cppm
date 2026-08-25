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
/// `detectWorktree` — the Zig module's git-worktree-cwd classifier — LANDED
/// at task 6137, but in `planar.git` (layer 1) rather than here. The Zig
/// original keeps it in this file because its `deriveFromCwd` USES it, to
/// redirect a worktree cwd to the parent repo for lookups. This port's
/// `derive_from_cwd` does not do that redirection, so a copy here would
/// have no caller inside the module and would only force `engine_identity`
/// to carry a subprocess dependency it does not use. `planar.git` is where
/// both actual consumers reach it from: the `cmd/planar` worktree gate,
/// and (when the redirection is ported) this module.
///
/// The REDIRECTION itself is still a residual gap, narrower than before and
/// named rather than silently dropped: `derive_from_cwd` below always
/// resolves the literal `cwd` argument. For the orchestrator's own
/// `.worktrees/<...>` convention this is invisible — the worktree lives
/// UNDER the project root, so the ordinary longest-prefix match already
/// finds the parent project. It is observable only for a linked worktree
/// created outside the project root, where a read resolves to no project
/// instead of the parent's scope.
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
/// yet"). This module's `resolve_for_write` covers
/// explicit-flag-wins-over-cwd-derivation, plus — since task 6134 — the
/// META-WORKSPACE arm (`resolve_meta_workspace_write_scope`), which the
/// original port omitted entirely. That omission was unreachable while no
/// meta workspace could be registered and would have become a silent wrong
/// answer the moment `workspace init` landed: a cwd that should refuse as
/// ambiguous would instead have resolved to whatever `derive_from_cwd`
/// happened to return. The specificity ranking and membership-aware
/// candidate sets remain cmd-layer read-path concerns and stay out.
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

/// @brief The two scope refs an ambiguous meta-workspace root offers the
/// operator. Mirrors zig's `MetaWriteResolution.ambiguous`
/// (zig/src/cmd/planar/scope.zig:474).
export struct meta_ambiguity {
  std::string assoc_scope; ///< `assoc:\<org-slug\>` — cross-repo / meta-level work.
  std::string repo_scope;  ///< `repo:\<project-slug\>` — root-repo work.
};

/// @brief What the meta-workspace probe concluded about a cwd. Mirrors
/// zig's `MetaWriteResolution` union.
export struct meta_write_resolution {
  /// @brief Which arm this is.
  enum class arm : std::uint8_t {
    none,       ///< Not inside any meta workspace. Fall through to `derive_from_cwd`.
    repo_scope, ///< Inside a meta workspace, BELOW its root: the member repo's scope.
    ambiguous,  ///< Standing exactly ON the meta root, which is also the root repo's path.
  };

  arm                           which = arm::none; ///< The arm.
  std::optional<std::string>    repo_scope;        ///< Set for `arm::repo_scope`.
  std::optional<meta_ambiguity> choices;           ///< Set for `arm::ambiguous`.
};

/// @brief A `resolve_for_write` failure, carrying the ambiguity detail
/// when there is one.
///
/// `scope_error` alone cannot express the meta-workspace refusal: that
/// message NAMES the two `--scope` values the operator may choose between,
/// and a caller handed a bare `scope_mismatch` would have to re-run the
/// probe purely to recover values the resolution already computed. Modelled
/// as a richer error rather than as a defaulted out-parameter deliberately
/// — a defaulted argument a caller forgets to pass is exactly the failure
/// shape task 6128 closed.
export struct write_scope_failure {
  scope_error                   code;      ///< The error bucket.
  std::optional<meta_ambiguity> ambiguity; ///< Set only for the meta-ambiguous refusal.
};

/// @brief Probe `cwd` against the registered meta workspaces.
///
/// Port of zig's `resolveMetaWorkspaceWriteScope`
/// (zig/src/cmd/planar/scope.zig:491). A meta workspace is an
/// `associations` row with `kind = 'org'` whose `config_json` carries
/// `workspace_shape = 'meta-repo'` and a `root_path`.
///
/// Two queries, in this order, and the order IS the rule:
///
///   1. Is `cwd` EXACTLY both the org's `root_path` and a member project's
///      `root_path`? Then the cwd names two different scopes equally well
///      and neither default is safe -> `ambiguous`.
///   2. Otherwise, find the longest member project root that `cwd` sits
///      under, where `cwd` is also under the org root -> that repo's scope.
///
/// Reads stay workspace-shaped; only WRITES are forced onto a concrete
/// repo. See zig's `resolveForWrite` doc comment.
/// @param conn An open, migrated database connection.
/// @param cwd The absolute working-directory path.
/// @return The conclusion, or `scope_error::query_failed` on a SQL failure.
export auto resolve_meta_workspace_write_scope(db::connection& conn, std::string_view cwd)
    -> std::expected<meta_write_resolution, scope_error>;

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
/// @return The resolution, or a `write_scope_failure`:
/// `scope_error::invalid_path` when falling back to `derive_from_cwd` on an
/// invalid `cwd`, `scope_error::query_failed` on a SQL failure, or
/// `scope_error::scope_mismatch` WITH `ambiguity` set when `cwd` is an
/// ambiguous meta-workspace root (see `resolve_meta_workspace_write_scope`).
export auto resolve_for_write(db::connection& conn, std::optional<std::string_view> scope_flag, std::string_view cwd)
    -> std::expected<write_scope_resolution, write_scope_failure>;

// =========================================================================
// The READ set (task 6141)
//
// Writes resolve to exactly one scope; reads resolve to a SET. The two are
// deliberately different rules, and `resolve_for_write` is not a special
// case of `resolve_read_scope_set` — a meta-workspace root is AMBIGUOUS for
// a write (refuse, name the two choices) and EXPANSIVE for a read (return
// the org association, its sibling project associations, and every member
// repo). Port of zig/src/cmd/planar/scope.zig's `resolveForReadSet` /
// `readScopeFilterSlugs`; it lives here rather than in `cmd/` because
// `resolve_for_write`'s precedence already moved into this module and
// splitting the pair across layers would put half the rule out of reach of
// this module's own tests.
// =========================================================================

/// @brief One member of a cwd-derived read set. Mirrors zig's `ReadScope`.
///
/// `id` is meaningless for `scope_kind::global` and is zero there —
/// matching the Zig original's defaulted field rather than an optional,
/// because `read_scope_filter_slugs` never reads it on that arm.
export struct read_scope {
  scope_kind   kind;   ///< Which kind of scope this member names.
  std::int64_t id = 0; ///< The row id; unused (and zero) for `global`.
};

/// @brief Resolve the read set for a listing verb.
///
/// An explicit `override` (the `--scope` value) returns exactly that one
/// parsed scope — and UNLIKE the write path, it IS validated against the
/// database here (`resolve_slug`), so `--scope nosuchthing` fails rather
/// than silently matching nothing. That asymmetry is the oracle's
/// (`resolveForWrite` threads the flag verbatim; `resolveForReadSet` calls
/// `resolveSlug`), and it is why an unknown `--scope` on `plan list`
/// reports a scope error while the same value on `plan create` does not.
///
/// Without an override, cwd drives the result, in this order:
///
///   1. A registered meta workspace containing `cwd`: exactly AT the org
///      root expands to the whole workspace; deeper inside a member project
///      narrows to that repo.
///   2. Otherwise every project/association/org whose root is a path
///      prefix of `cwd` becomes a candidate, and the MOST SPECIFIC wins —
///      by kind rank (project 1, ad-hoc/personal 2, client 3, repo 4, org
///      5, anything else 6; lower is more specific), then by longest root
///      path. An `org` winner expands to the whole workspace.
///
/// An EMPTY result is a meaningful answer, not an error: it means the cwd
/// pinned no scope (nothing matched, or two candidates tied at the top).
/// Callers MUST refuse rather than treating it as "no filter", because an
/// unfiltered listing of every scope in the database is exactly the
/// plausible-but-wrong output this set exists to prevent.
/// @param conn An open, migrated database connection.
/// @param cwd The absolute working-directory path.
/// @param override The `--scope` flag's raw value, when passed.
/// @return The read set (possibly empty), or `scope_error::slug_not_found`
/// when `override` names no row, or `scope_error::query_failed`.
export auto resolve_read_scope_set(db::connection& conn, std::string_view cwd, std::optional<std::string_view> override)
    -> std::expected<std::vector<read_scope>, scope_error>;

/// @brief Turn a read set into the slug labels a list filter takes.
///
/// `global` becomes the literal `"global"`; an association becomes
/// `assoc:<slug>`; a repo becomes `repo:<slug>`. Mirrors zig's
/// `readScopeFilterSlugs`, including its `coalesce(slug,'')` — a row with a
/// NULL slug yields `assoc:` / `repo:` rather than being dropped, so a
/// malformed row surfaces downstream as an unresolvable scope instead of
/// silently shortening the filter.
/// @param conn An open, migrated database connection.
/// @param scopes The read set to label.
/// @return One slug per input, in order, or `scope_error::slug_not_found`
/// when a member's row has vanished, or `scope_error::query_failed`.
export auto read_scope_filter_slugs(db::connection& conn, std::span<const read_scope> scopes)
    -> std::expected<std::vector<std::string>, scope_error>;

} // namespace planar::engine::identity
