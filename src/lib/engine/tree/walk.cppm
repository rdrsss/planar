/// @file walk.cppm
/// @brief `planar.engine.tree.walk` — the hierarchical entity walker and
/// its two renderers behind `planar tree` (plan 996, task 6278).
///
/// Behavior-preserving port (D2) of zig/src/engine/tree.zig — `build`,
/// `renderText`, and the `Node` / `Filter` vocabulary they exchange, plus
/// the `jsonStringify` branch that is the `--json` wire format.
///
/// ## The walk
///
/// Three relationship channels, exactly as the oracle walks them:
///   - `plans.parent_plan_id`            plan -> plan
///   - `tasks.plan_id` / `parent_task_id` plan -> task -> subtask
///   - `entity_links(derives-from)`       plan -> artifact/decision/
///                                        test_scenario/question
///
/// Read-only: no writes, no transactions, no audit rows.
///
/// ## The activity rollup lives in layer 1 (task 6282)
///
/// The oracle's walker calls `engine.runtime.agentactivity.summary
/// .forEntity` to fold a compact per-entity activity rollup onto every
/// entity node. In C++ that function's natural home is
/// `planar_engine_runtime`, and `engine_tree -> engine_runtime` is an
/// `engine_* -> engine_*` edge that cmake/architecture.cmake FATALs on at
/// configure time (D15/D18) — still true, and pinned by a probe: adding
/// that edge fails configure with "layer 2, which is not strictly
/// downward".
///
/// So `for_entity` was a deliberate, documented DUPLICATE of the oracle's
/// three queries. It is no longer: task 6282 extracted it downward to the
/// layer-1 `planar.activity_rollup`, which both this bucket and
/// `engine_runtime` can reach without depending on each other — D19's
/// prescribed move, the same one task 6089 made for scope resolution.
///
/// It was done while `engine_runtime` still had no copy of its own
/// (it ports the rollup's two SIBLINGS, `recent_actions_for_entity` and
/// `claim_transitions_for_entity`, for `audit trail`, but not the compact
/// rollup). With one implementation this was a MOVE; with two it would
/// have been a three-way merge.
///
/// ## `--sort` IS WIRED (task 6281); IT USED TO BE INERT
///
/// The oracle declared `Filter.sort` and a `--sort` flag and NEVER read the
/// field: every query in tree.zig ordered by `id` (or `slug` for scope
/// roots) unconditionally, so `tree --sort updated` and `tree --sort bogus`
/// were both byte-identical to a bare `tree` and an unrecognised key did not
/// refuse. That was reproduced under D2; decision 1067 ended the obligation
/// and this task wired it.
///
/// THE SORT IS APPLIED AFTER THE WALK, not pushed into the queries. Every
/// query keeps its `order by id`, which makes id the STABLE TIE-BREAK for
/// every other key — two rows sharing an `updated_at` come out in id order,
/// deterministically. Pushing the key into ~10 separate statements would
/// have bought nothing and risked each one drifting.
///
/// `updated` and `created` sort DESCENDING (most recent first), which is what
/// an operator scanning a tree for recent movement wants; `id` and
/// `unsorted` are ascending/insertion order. There is no `--reverse` on this
/// verb, so the direction is fixed per key rather than composable.
///
/// SCOPE ROOTS ARE NEVER REORDERED. They carry no timestamps (both fields are
/// empty strings on a scope node), so sorting them by one would be sorting by
/// nothing. `--all-scopes` keeps its scope order under every key.
///
/// THE SORT IS SINGLE-LEVEL: it orders each scope root's DIRECT children, the
/// top-level plans. It does not descend. A plan's children are a mixed-kind
/// level -- tasks alongside artifacts, decisions, scenarios and questions --
/// and re-ordering that by timestamp interleaves the kinds rather than
/// clarifying anything. Separately, a recursive version made `build` throw;
/// see task 6757 and the note on `sort_level`.
///
/// ## Empty flag values mean three DIFFERENT things on this one verb
///
/// Captured, not assumed (the sibling-inference trap this milestone keeps
/// paying for):
///   - `--kind ""`   -> REFUSES, exit 2, `error: unknown kind ''`
///   - `--status ""` -> exit 0, matches nothing (empty tree, zero counts)
///   - `--scope ""`  -> exit 0, treated as GLOBAL
/// Nothing here may be inferred from any other verb's empty-value rule.
module;

export module planar.engine.tree.walk;

import std;
import planar.activity_rollup;
import planar.db;

namespace planar::engine::tree {

/// @brief The compact per-entity agent-activity rollup folded onto every
/// entity node, or absent when the entity has neither actions nor claims.
///
/// Absent is NOT the same as zeroed: the text renderer prints no sub-line
/// and the JSON renderer omits the `activity_summary` key entirely, so a
/// consumer never sees an empty placeholder.
/// Aliased rather than redeclared: the type moved to the layer-1
/// `planar.activity_rollup` at task 6282, and every existing consumer spells
/// it `tree::activity_summary`. Keeping the name here makes the extraction a
/// pure move for callers.
export using activity_summary = planar::activity_rollup::activity_summary;

/// @brief One entity in the tree.
///
/// A single struct carries both SCOPE ROOTS (`kind == "scope"`) and entity
/// rows, exactly as the oracle's `Node` does, because the two are rendered
/// by the same recursion. The unused half is empty rather than absent —
/// but the JSON renderer branches on `kind` and emits only the relevant
/// fields, which is the whole point of the oracle's custom
/// `jsonStringify` (task 2377: the default struct serializer leaked
/// `id: 0`, `slug: ""`, `status: ""` onto scope roots).
export struct node {
  std::string                     kind;          ///< `scope`, `plan`, `task`, `question`, `scenario`, `decision`, `artifact`.
  std::int64_t                    id = 0;        ///< Row id; 0 for scope roots.
  std::string                     title;         ///< Display title; the scope label for scope roots.
  std::string                     slug;          ///< Set for plans; empty otherwise.
  std::string                     status;        ///< Row status; empty for scope roots.
  std::int64_t                    priority = 0;  ///< Set for tasks; 0 otherwise.
  std::string                     artifact_kind; ///< Set for artifacts; empty otherwise.
  std::string                     scope_kind;    ///< Set for scope roots; empty otherwise.
  std::optional<std::int64_t>     scope_id;      ///< Set for non-global scope roots.
  std::string                     scope_label;   ///< Set for scope roots; empty otherwise.
  std::string                     created_at;    ///< `coalesce(created_at,'')`.
  std::string                     updated_at;    ///< `coalesce(updated_at,'')`.
  std::vector<node>               children;      ///< Ordered children, DB insertion order (id ASC).
  std::optional<activity_summary> activity;      ///< Rollup, or absent. NEVER set on scope roots.
};

/// @brief How a level's siblings are ordered (task 6281).
///
/// Applied after the walk; see this file's header. `id` is the default and
/// matches every query's own `order by id`, so it is also the stable
/// tie-break under `updated` and `created`.
export enum class sort_key : std::uint8_t {
  id,       ///< Ascending row id. The default, and every other key's tie-break.
  updated,  ///< `updated_at` DESCENDING, most recent first.
  created,  ///< `created_at` DESCENDING, most recent first.
  unsorted, ///< Leave the walk's own order untouched.
};

/// @brief Parse a `--sort` value.
/// @param text The raw flag value.
/// @return The key, or unset when `text` names none.
export auto sort_key_from_text(std::string_view text) -> std::optional<sort_key>;

/// @brief Which entities the walk keeps, and how siblings are ordered.
export struct tree_filter {
  std::optional<std::string>  scope;              ///< Scope slug; unset -> global. Ignored when `all_scopes`.
  bool                        all_scopes = false; ///< Walk global + every association + every project.
  std::int64_t                max_depth = -1; ///< Max depth; ANY value <= 0 is unbounded (`--depth 0` == `--depth -1`, captured).
  std::vector<std::string>    kinds;          ///< Restrict to these kinds; empty -> all.
  std::vector<std::string>    statuses;       ///< Restrict to these statuses; empty -> any.
  std::optional<std::int64_t> root_plan_id;   ///< Start from this top-level plan only.
  sort_key                    sort = sort_key::id; ///< Sibling ordering (task 6281).
};

/// @brief Error surface for the walk.
export enum class tree_error : std::uint8_t {
  query_failed,      ///< An underlying SQL statement failed.
  unsupported_scope, ///< The scope slug named an unsupported form.
  slug_not_found,    ///< The scope slug resolved to no row.
  unknown_kind,      ///< A `kinds` entry is not one of `valid_kinds`.
};

/// @brief The six kinds `--kind` accepts, in the oracle's declaration order.
/// @return The accepted kind tokens. `test_scenario` is deliberately ABSENT:
/// it is the DB spelling, and the flag spelling is `scenario`.
export auto valid_kinds() -> std::span<const std::string_view>;

/// @brief Is `kind` one of `valid_kinds()`?
/// @param kind The candidate kind.
/// @return `true` when accepted.
export auto is_valid_kind(std::string_view kind) -> bool;

/// @brief Walk the entity graph and return the scope roots.
///
/// Always returns at least one root when the filter names a resolvable
/// scope: an empty scope yields a root with no children, NOT an empty
/// vector and NOT an error. `planar tree` on an empty scope is exit 0.
/// @param conn An open, migrated connection.
/// @param filter The walk filter.
/// @return The scope roots, or the first error encountered.
export auto build(db::connection& conn, const tree_filter& filter) -> std::expected<std::vector<node>, tree_error>;

/// @brief Render the indented box-drawing tree.
///
/// Unicode charset, dirs-first grouping (plans, then tasks, then the
/// rest), one blank line between roots and before the summary footer.
/// @param roots The scope roots.
/// @return The COMPLETE payload including its trailing newline; the
/// handler writes it verbatim and appends nothing.
export auto render_text(std::span<const node> roots) -> std::string;

/// @brief Render the `--json` wire format.
///
/// A SINGLE root serialises as a JSON object; two or more serialise as a
/// JSON array. That asymmetry is the oracle's and is load-bearing for
/// consumers, so it is reproduced rather than normalised.
/// @param roots The scope roots.
/// @return The COMPLETE payload including its trailing newline.
export auto render_json(std::span<const node> roots) -> std::string;

/// @brief The refusal body for an unrecognised `--kind`.
/// @param kind The rejected value, interpolated verbatim (the empty string
/// is a real, reachable case: `--kind ""` refuses with `''`).
/// @return The body FRAGMENT, no trailing newline, for `error: <body>`.
export auto render_unknown_kind(std::string_view kind) -> std::string;

} // namespace planar::engine::tree
