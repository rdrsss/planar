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
/// ## Why the activity rollup is duplicated here rather than imported
///
/// The oracle's walker calls `engine.runtime.agentactivity.summary
/// .forEntity` to fold a compact per-entity activity rollup onto every
/// entity node. In C++ that function's home is `planar_engine_runtime`,
/// and `engine_tree -> engine_runtime` is an `engine_* -> engine_*` edge,
/// which cmake/architecture.cmake FATALs on at configure time (D15/D18).
///
/// So `for_entity` below is a deliberate, documented DUPLICATE of the
/// oracle's three queries rather than a call into the runtime bucket. It
/// is not a rewrite: the SQL, the ordering, the `coalesce` choices and the
/// no-actions-but-claims fallback are transcribed verbatim. Note that
/// `planar_engine_runtime` ports the rollup's two SIBLINGS
/// (`recent_actions_for_entity`, `claim_transitions_for_entity`) for
/// `audit trail` but NOT the compact rollup itself, so this is the tree's
/// first and currently only copy — there is no second one to drift from
/// today. The principled fix is a D19 extraction of the rollup to layer 1,
/// which is the same move task 6089 made for scope resolution; it is a
/// task of its own and NOT smuggled into a port cycle.
///
/// ## `--sort` IS INERT, AND THAT IS THE ORACLE'S BEHAVIOR
///
/// The oracle declares `Filter.sort` and a `--sort` flag, and **never
/// reads the field**: every query in tree.zig orders by `id` (or by `slug`
/// for the scope roots) unconditionally. Verified by capture, not by
/// reading: `tree --sort updated` and `tree --sort bogus` both produce
/// output byte-identical to a bare `tree`, and an unrecognised sort key
/// does NOT refuse (exit 0). This module therefore carries no `sort`
/// field at all — a dead member reproduced faithfully is still dead code,
/// and the observable contract (accept the flag, ignore it) is preserved
/// at the handler. Recorded as an oracle defect for a follow-up task
/// rather than silently "fixed" here: implementing a sort would be a
/// behavior CHANGE, which D2 forbids in a port.
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
import planar.db;

namespace planar::engine::tree {

/// @brief The compact per-entity agent-activity rollup folded onto every
/// entity node, or absent when the entity has neither actions nor claims.
///
/// Absent is NOT the same as zeroed: the text renderer prints no sub-line
/// and the JSON renderer omits the `activity_summary` key entirely, so a
/// consumer never sees an empty placeholder.
export struct activity_summary {
  std::string  latest_action_kind;     ///< Latest `agent_actions.action_kind`; empty on the claim-only fallback.
  std::string  latest_vendor;          ///< Vendor of the latest action, or of the latest claim on the fallback.
  std::string  last_event_at;          ///< ISO8601 stamp of the latest action or claim transition.
  std::int64_t active_claim_count = 0; ///< Count of active, unexpired claims.
};

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

/// @brief Which entities the walk keeps.
///
/// Deliberately carries NO `sort` member; see this file's header for why
/// the oracle's dead `Filter.sort` is not reproduced.
export struct tree_filter {
  std::optional<std::string>  scope;              ///< Scope slug; unset -> global. Ignored when `all_scopes`.
  bool                        all_scopes = false; ///< Walk global + every association + every project.
  std::int64_t                max_depth = -1; ///< Max depth; ANY value <= 0 is unbounded (`--depth 0` == `--depth -1`, captured).
  std::vector<std::string>    kinds;          ///< Restrict to these kinds; empty -> all.
  std::vector<std::string>    statuses;       ///< Restrict to these statuses; empty -> any.
  std::optional<std::int64_t> root_plan_id;   ///< Start from this top-level plan only.
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
