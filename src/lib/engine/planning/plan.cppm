/// @file plan.cppm
/// @brief `planar.engine.planning.plan` — Plan entity CRUD plus the
/// plan-status auto-promotion invariant (tech-spec § "engine buckets",
/// plan 996 task cpp-planning-verbs; docs/concepts.md:459 "plan 304").
///
/// Behavior-preserving port (D2) of a SUBSET of zig/src/engine/planning/plan.zig:
///   - `create`, `show`, `list`, `update` CRUD against the `plans` table.
///   - `recompute_status` — the plan-304 auto-promotion aggregate roll-up.
///
/// NOT ported (see this task's coder report for the full scoping
/// rationale): `listTouching` and the `task touches` path-level surface
/// (a separate feature, not required by this task's acceptance
/// criteria); `renderText`/`renderListText` (a `cmd/`-layer output
/// concern — this task explicitly excludes a `cmd/` binary);
/// `policy.audit.record` calls and the `session_entries` forensic-note
/// side effect (`emitStatusSessionEntry`) — there is no
/// `planar.engine.policy.audit` module in the C++ tree yet (same
/// omission `association.cppm` already documented), and the session note
/// is itself best-effort/error-swallowed in the Zig original, so cutting
/// it changes no observable return value. `wouldCreateParentCycle`'s
/// `InvalidParentCycle` guard on `parent_plan_id` reassignment IS ported
/// — it is cheap and load-bearing for hierarchy integrity.
///
/// Scope handling: mirrors zig — when `scope` is provided in
/// `create_args`/`update_args`, it is resolved to a `(kind, id)` pair
/// against the `associations`/`projects` tables (`global` /
/// `repo:<slug>` / `assoc:<slug>` / bare-association forms). This is
/// deliberately a LOCAL, module-private re-implementation of
/// `planar.engine.identity.scope`'s `resolve_slug` algorithm, NOT a
/// dependency on the `engine_identity` module — see this file's
/// CMakeLists.txt for why: a layer-2-to-layer-2 (`engine_*` to
/// `engine_*`) dependency edge is forbidden by D15/D18
/// (`cmake/architecture.cmake`'s same-layer prohibition), contradicting
/// what this task's brief claimed. The cross-scope guard
/// (`check_scope_guard`/`guard_write`) is deliberately NOT called from
/// this module — see this task's coder report, "scope guard left
/// opt-in" (task 6075 is an unresolved operator decision on whether the
/// guard should apply to every mutation verb; wiring it in here would
/// silently resolve that decision).
module;

export module planar.engine.planning.plan;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief Mirrors zig's plan.zig `ScopeKind` (same wire vocabulary as
/// `planar.engine.identity.scope`'s `scope_kind`, kept as a distinct type
/// here because it is what this module's rows actually store/read —
/// mirrors the Zig original's own per-module `ScopeKind` duplication).
export enum class plan_scope_kind : std::uint8_t {
  repo,
  association,
  global,
};

/// @brief A plan's lifecycle status. Mirrors zig's plan.zig `Status`.
export enum class plan_status : std::uint8_t {
  draft,
  active,
  paused,
  done,
  abandoned,
};

/// @brief Parse a `status` column value / `--status` flag value.
/// @param s The raw text to parse.
/// @return The parsed status, or unset for an unrecognized string.
export auto plan_status_from_text(std::string_view s) -> std::optional<plan_status>;

/// @brief Render `s` as the wire/column text form.
/// @param s The status to render.
/// @return The wire/column text form.
export auto plan_status_to_text(plan_status s) -> std::string_view;

/// @brief One row from the `plans` table. Mirrors zig's plan.zig `Plan`.
export struct plan {
  std::int64_t                id;             ///< The row's id.
  plan_scope_kind             scope_kind;     ///< Which kind of scope this plan belongs to.
  std::optional<std::int64_t> scope_id;       ///< The scope's row id, unset for global.
  std::string                 title;          ///< Display title.
  std::string                 slug;           ///< Filesystem-safe identifier, unique per parent scope.
  std::optional<std::string>  summary;        ///< Optional free-text summary.
  plan_status                 status;         ///< Current lifecycle status.
  std::optional<std::int64_t> parent_plan_id; ///< Parent plan's row id, unset for an anchor plan.
  std::string                 created_at;     ///< Row creation timestamp.
  std::string                 updated_at;     ///< Row last-update timestamp.
};

/// @brief Arguments to `create`. Mirrors zig's plan.zig `CreateArgs`.
export struct plan_create_args {
  std::string                 title;                       ///< The plan's title (required).
  std::optional<std::string>  slug;                        ///< Derived from `title` if unset.
  std::optional<std::string>  summary;                     ///< Optional free-text summary.
  plan_status                 status = plan_status::draft; ///< Initial status.
  std::optional<std::int64_t> parent_plan_id;              ///< Parent plan's row id, when this is a child plan.
  std::optional<std::string>  scope;                       ///< Scope slug accepted by `identity::resolve_slug`.
};

/// @brief Arguments to `update`. Mirrors zig's plan.zig `UpdateArgs`.
export struct plan_update_args {
  std::optional<std::string>  title;                ///< New title, when set.
  std::optional<std::string>  slug;                 ///< New slug, when set.
  std::optional<std::string>  summary;              ///< New summary, when set.
  std::optional<plan_status>  status;               ///< New status, when set (validated via the transition matrix).
  std::optional<std::int64_t> parent_plan_id;       ///< New parent plan id, when set.
  bool                        clear_parent = false; ///< When true (and `parent_plan_id` unset), clears the parent.
  std::optional<std::string>  scope;                ///< Scope slug accepted by `identity::resolve_slug`.
};

/// @brief Filter for `list`. Mirrors zig's plan.zig `ListFilter` (minus
/// the multi-scope `scopes` array — this task ports the single-`scope`
/// filter form only; the handler-facing multi-scope walk is a `cmd/`-layer
/// concern out of scope for this task).
export struct plan_list_filter {
  std::vector<plan_status>    statuses;       ///< Match any of these statuses; empty matches every status.
  std::optional<std::int64_t> parent_plan_id; ///< Match only plans with this parent, when set.
  std::optional<std::string>  scope;          ///< Scope slug accepted by `identity::resolve_slug`.
};

/// @brief Error surface for every fallible operation in this module.
export enum class plan_error : std::uint8_t {
  not_found,
  slug_conflict,
  slug_not_found,
  invalid_parent_cycle,
  illegal_transition,
  unknown_status,
  query_failed,
};

/// @brief Create a new plan.
/// @param conn An open, migrated database connection.
/// @param args The plan's title (required) plus optional slug/summary/status/parent/scope.
/// @return The created row, or `plan_error::slug_conflict` on a slug
/// collision, `plan_error::slug_not_found` if `scope` does not resolve
/// (unknown association/repo slug), or `plan_error::query_failed`.
export auto create_plan(db::connection& conn, const plan_create_args& args) -> std::expected<plan, plan_error>;

/// @brief Look up a plan by id.
/// @param conn An open, migrated database connection.
/// @param id The plan's row id.
/// @return The row, or `plan_error::not_found`, or `plan_error::query_failed`.
export auto show_plan(db::connection& conn, std::int64_t id) -> std::expected<plan, plan_error>;

/// @brief List plans matching `filter`, ordered by id.
/// @param conn An open, migrated database connection.
/// @param filter Status/parent/scope filters (all optional; empty matches everything).
/// @return The matching rows, or `plan_error::slug_not_found` if
/// `filter.scope` does not resolve, or `plan_error::query_failed`.
export auto list_plans(db::connection& conn, const plan_list_filter& filter) -> std::expected<std::vector<plan>, plan_error>;

/// @brief Update a plan. Mirrors zig's plan.zig `update`: builds a
/// dynamic SET clause from whichever fields of `patch` are set; a
/// no-op patch (nothing set) is a successful read-only `show`.
///
/// A `patch.status` change is validated via
/// `planar.engine.planning.transitions`' plan arm before the write
/// (`plan_error::illegal_transition` / `plan_error::unknown_status` on
/// refusal). A `patch.parent_plan_id` reassignment is checked against
/// the existing ancestor chain first (`plan_error::invalid_parent_cycle`
/// if it would create a cycle).
/// @param conn An open, migrated database connection.
/// @param id The plan's row id.
/// @param patch The fields to change.
/// @return The updated (or unchanged) row, or one of the errors above.
export auto update_plan(db::connection& conn, std::int64_t id, const plan_update_args& patch) -> std::expected<plan, plan_error>;

/// @brief The outcome of a `recompute_status` call. Mirrors zig's
/// plan.zig `RecomputeResult`.
export struct recompute_result {
  std::int64_t plan_id;       ///< The plan that was recomputed.
  plan_status  status_before; ///< Status before the recompute.
  plan_status  status_after;  ///< Status after the recompute (equal to `status_before` when not flipped).
  bool         flipped;       ///< True when the status was actually changed.
};

/// @brief Recompute the plan-304 auto-promotion status for THIS plan
/// only (single-plan; never walks `parent_plan_id` — mirrors zig's
/// documented D-recompute-single-plan semantics).
///
/// Applies the aggregate transition matrix (mirrors Go's recompute.go /
/// zig's `computeTarget` exactly):
///
///   from \\ aggregate    | empty | all todo | any active | all terminal
///   ---------------------+-------+----------+------------+--------------
///   draft                | -     | -        | active     | done*
///   active               | -     | -        | -          | done*
///   paused               | -     | -        | -          | -   (operator)
///   done                 | -     | active   | active     | -
///   abandoned             | -     | -        | -          | -   (terminal)
///
///   * anchor plans (`parent_plan_id IS NULL`) NEVER auto-promote to
///     `done`; they auto-promote to `active` at most.
///
/// Deliberately bypasses `planar.engine.planning.transitions`' plan arm —
/// this is an engine-internal aggregate roll-up whose target is computed
/// by the matrix above and can only emit transitions the aggregate matrix
/// considers valid; it is not an operator transition (mirrors zig's own
/// documented bypass, plan.zig:760-771).
///
/// Idempotent: calling on an already-correct plan is a no-op (no write,
/// `flipped=false`).
///
/// @param conn An open, migrated database connection.
/// @param plan_id The plan to recompute.
/// @return The recompute outcome, or `plan_error::not_found` /
/// `plan_error::query_failed`.
export auto recompute_status(db::connection& conn, std::int64_t plan_id) -> std::expected<recompute_result, plan_error>;

/// @brief Render one plan as the operator-facing key/value block.
///
/// Ports zig/src/engine/planning/plan.zig's `renderText` byte for byte,
/// including the two conditional lines: `parent:` appears only when
/// `parent_plan_id` is set and `summary:` only when `summary` is set, and
/// in THAT order (parent before summary), which is not the struct's field
/// order. The `scope:` value is the scope kind, with `:<id>` appended when
/// `scope_id` is set — so `global`, `association:1`, `repo:1`.
/// @param p The plan to render.
/// @return The complete block, INCLUDING its trailing newline. The caller
/// writes it verbatim and appends nothing.
export auto render_text(const plan& p) -> std::string;

/// @brief Render one plan as the single-line JSON object.
///
/// Field order is the `plan` struct's declaration order, because the
/// oracle's JSON path is `std.json.Stringify.value` over the Zig `Plan`
/// struct and Zig serializes fields in declaration order. `scope_id`,
/// `summary` and `parent_plan_id` render as `null` when unset.
/// @param p The plan to render.
/// @return The JSON object with NO trailing newline — a fragment the
/// caller terminates (the oracle's `output.emit` prints `"\n"` after
/// stringifying).
export auto render_json(const plan& p) -> std::string;

} // namespace planar::engine::planning
