/// @file scope_ref.cppm
/// @brief `planar.scope_ref` — layer-1 scope-reference parsing and
/// normalisation (decision D19, plan 996 task 6089).
///
/// PROBLEM this module closes: `engine_identity`'s `resolve_slug` (the
/// `global` / `repo:<slug>` / `assoc:<slug>` / bare-association scope-ref
/// grammar, plus its two-table DB lookup) and its `normalize_assoc`
/// helper were needed by `engine_planning` (plan.cpp/task.cpp's
/// `resolve_scope_or_global`). D18 keeps the same-layer prohibition for
/// layers 2 (`engine_*`) and 3 (`cmd_*`) — engine buckets remain siblings
/// that cannot depend on each other — so `engine_planning` could not
/// simply `import planar.engine.identity.scope`. Before this task, that
/// left the algorithm duplicated verbatim (modulo return-type names) in
/// `engine_identity/scope.cpp`, `engine_planning/plan.cpp`, and
/// `engine_planning/task.cpp`, with no mechanism to catch silent drift if
/// one copy changed and the others did not.
///
/// D19's fix: extract the genuinely shared primitive into a NEW layer-1
/// module (classified by name per `cmake/architecture.cmake` — anything
/// not `cmd_*`/`engine_*` is layer 1) that any engine bucket may depend
/// on, same as `db`. D18's same-layer prohibition is UNCHANGED —
/// `engine_identity` and `engine_planning` still cannot depend on each
/// other; they now both depend downward on this module instead.
///
/// Scope of the extraction (judgement call, task 6089 coder report):
/// the FULL resolution — grammar parsing AND the DB lookup — moves here,
/// not just the pure string-parsing half. The bulk of what was actually
/// duplicated across the three call sites was the `prepare`/`bind`/
/// `step`/error-mapping boilerplate for the `projects` and `associations`
/// lookups, not the branch-on-prefix parsing alone; a "parsing only"
/// split would have left that boilerplate — the actual drift risk D19
/// exists to remove — duplicated in both `engine_identity` and
/// `engine_planning`. Depending on `planar.db` from a layer-1 module is
/// not "dragging a db dependency somewhere it does not belong": `db`
/// already sits at layer 1, `engine_identity` and `engine_planning`
/// already depend on it directly, and a layer-1 module whose entire job
/// is a DB-backed resolution primitive is exactly where that dependency
/// belongs (D15/D18's same-layer-1 carve-out already permits `cli` ->
/// `core`-shaped edges of this kind).
///
/// `normalize_assoc` is exported separately (not just used internally by
/// `resolve`) because `engine_identity`'s `check_scope_guard` calls it
/// directly on two already-resolved scope labels, with no DB lookup
/// involved — that is a second, independent duplication site this module
/// also closes.
///
/// `association.cpp`'s `slugify_path_segment` was assessed against this
/// same extraction (task brief, D19 body: "association.cpp already
/// carries the same risk") and deliberately NOT moved here: verified via
/// `grep -rn "slugify_path_segment\|auto slugify" src/lib` before this
/// change that no second copy exists anywhere in the tree today.
/// `plan.cpp`'s `slugify(title)` is superficially similar (both lowercase
/// + collapse-to-dash) but is a DIFFERENT algorithm serving a different
/// purpose (plan-title -> slug, not path-segment -> slug) — it strips at
/// most one trailing dash (`if`) with no empty-string fallback, where
/// `slugify_path_segment` strips ALL trailing dashes (`while`) and falls
/// back to `"_"` on an empty result. Extracting either now, with only one
/// real call site each, would be exactly the "might be useful later"
/// speculative abstraction the coder guide rules out; nothing to extract
/// today.
module;

export module planar.scope_ref;

import std;
import planar.db;

namespace planar::scope_ref {

/// @brief Which kind of scope a scope-ref slug names. Mirrors
/// `planar.engine.identity.scope`'s `scope_kind` (kept distinct rather
/// than shared, so consuming buckets are free to define their own
/// entity-scoped enum without importing this module's type into their
/// public API).
export enum class scope_kind : std::uint8_t {
  global,      ///< No project or association filter.
  association, ///< A named `associations` row.
  repo,        ///< A `projects` row, referenced as `repo:\<slug\>`.
};

/// @brief A resolved `(kind, id)` pair. `id` is unset for
/// `scope_kind::global`.
export struct slug_ref {
  scope_kind                  kind; ///< Which kind of scope this ref names.
  std::optional<std::int64_t> id;   ///< The row id, unset for `scope_kind::global`.
};

/// @brief Error surface for this module's fallible operations.
export enum class error : std::uint8_t {
  query_failed,   ///< An underlying SQL statement failed.
  slug_not_found, ///< The slug did not resolve to any row.
};

/// @brief Strip an optional leading `"assoc:"` prefix from a scope label.
/// Pure, no I/O.
///
/// @param scope The scope label to normalise.
/// @return `scope` with any leading `"assoc:"` removed, else `scope` unchanged.
export auto normalize_assoc(std::string_view scope) -> std::string_view;

/// @brief Resolve a scope-ref slug to a `(kind, id)` pair.
///
/// Accepted forms:
///   "global"       -> {kind: global,      id: unset}
///   "\<slug\>"       -> {kind: association, id: \<assoc_id\>}
///   "assoc:\<slug\>" -> {kind: association, id: \<assoc_id\>}
///   "repo:\<slug\>"  -> {kind: repo,        id: \<project_id\>}
///
/// @param conn An open, migrated database connection.
/// @param slug The scope-ref slug to resolve.
/// @return The resolved ref, or `error::slug_not_found` when the
/// association/project slug does not exist, or `error::query_failed` on a
/// SQL failure.
export auto resolve(db::connection& conn, std::string_view slug) -> std::expected<slug_ref, error>;

} // namespace planar::scope_ref
