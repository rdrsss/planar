/// @file task.cppm
/// @brief `planar.engine.planning.task` — Task entity CRUD plus status
/// transitions, and the caller side of the plan-304 auto-promotion
/// invariant (tech-spec § "engine buckets", plan 996 task
/// cpp-planning-verbs).
///
/// Behavior-preserving port (D2) of a SUBSET of zig/src/engine/planning/task.zig:
///   - `create`, `show`, `list`, `update` CRUD against the `tasks` table.
///   - `mark_done`, `mark_cancelled`, `mark_blocked`, `reopen` — the
///     dedicated status-transition verbs.
///   - Every one of these six write paths (create, update, mark_done,
///     mark_cancelled, mark_blocked, reopen) calls
///     `planar.engine.planning.plan`'s `recompute_status` on the
///     affected plan(s), same as the Zig original — this is the
///     plan-304 auto-promotion invariant's caller side.
///
/// NOT ported (see this task's coder report for the full scoping
/// rationale):
///   - The active-work-claim TOCTOU guard (`hasActiveClaimOnTask`,
///     `error.TaskClaimed`, decision 533 / task 4165). `agent_work_claims`
///     is a runtime/agent-activity concern from a different bucket
///     (`engine.runtime.agentactivity`) that has not landed in the C++
///     tree yet; wiring a guard against a table no C++ module writes to
///     would be dead defensive code with no way to exercise the guarded
///     branch. `force` is still accepted on the transition verbs (it
///     still bypasses the transition MATRIX, which is this task's actual
///     concern) — only the claim-existence check itself is cut.
///   - ~~`due_at` format validation~~ — PORTED at task 6135, when `task
///     add` was wired and brought a `--due` flag with it. The original cut
///     reasoned that format enforcement was a `cmd/`-layer concern; the
///     oracle disagrees — `parseDueAt` is called from inside
///     `task.zig`'s `create` (:323) and `update` (:731), after scope
///     resolution, so both the error AND its ordering are engine
///     behaviour. See `task_error::invalid_due_at`.
///   - The `task_touch_paths` / `entity_links … relationship='touches'`
///     surface (`touchedRepoIDs`, `TouchPath`, `addTouchPath`,
///     `removeTouchPath`, `touchedPaths`, `listTouching`) — a separate
///     feature (polyrepo touches), not required by this task's
///     acceptance criteria.
///   - `renderText`/`renderListText` — `cmd/`-layer output, excluded per
///     this task's CONSTRAINTS ("no cmd/ binary").
///   - `policy.audit.record` calls — no `planar.engine.policy.audit`
///     module exists in the C++ tree yet (same omission `plan.cppm`
///     already documents).
///
/// `mark_blocked` DOES still insert the `entity_links` `task -> task`
/// `depends-on` edge (cheap, load-bearing for the "what is this task
/// blocked on" query) and `reopen`/forced-terminal-escape update DO still
/// insert `task_reopens` audit rows (cheap, and the acceptance criteria
/// explicitly calls out "status transition rules" as load-bearing).
///
/// Scope handling and the cross-scope guard: identical policy to
/// `plan.cppm` — `scope` is resolved via a LOCAL duplicate of
/// `identity::resolve_slug`'s algorithm when provided (this module does
/// NOT depend on `engine_identity` — see plan.cppm's CMakeLists.txt
/// comment for why a layer-2-to-layer-2 edge is forbidden); the
/// cross-scope guard is deliberately NOT wired in (task 6075 is
/// unresolved; see this task's coder report).
module;

export module planar.engine.planning.task;

import std;
import planar.db;
import planar.engine.planning.plan;

namespace planar::engine::planning {

/// @brief Mirrors zig's task.zig `ScopeKind`.
export enum class task_scope_kind : std::uint8_t {
  repo,
  association,
  global,
};

/// @brief A task's lifecycle status. Mirrors zig's task.zig `Status`.
export enum class task_status : std::uint8_t {
  todo,
  doing,
  blocked,
  done,
  cancelled,
};

/// @brief Parse a `status` column value / `--status` flag value.
/// @param s The raw text to parse.
/// @return The parsed status, or unset for an unrecognized string.
export auto task_status_from_text(std::string_view s) -> std::optional<task_status>;

/// @brief Render `s` as the wire/column text form.
/// @param s The status to render.
/// @return The wire/column text form.
export auto task_status_to_text(task_status s) -> std::string_view;

/// @brief One row from the `tasks` table. Mirrors zig's task.zig `Task`.
export struct task {
  std::int64_t                id;             ///< The row's id.
  task_scope_kind             scope_kind;     ///< Which kind of scope this task belongs to.
  std::optional<std::int64_t> scope_id;       ///< The scope's row id, unset for global.
  std::optional<std::int64_t> plan_id;        ///< The owning plan's row id, when set.
  std::optional<std::int64_t> parent_task_id; ///< The parent task's row id, when set.
  std::string                 title;          ///< Display title.
  std::optional<std::string>  body;           ///< Optional free-text body.
  std::optional<std::string>  slug;           ///< Optional stable slug, cited as `task:<slug>`.
  task_status                 status;         ///< Current lifecycle status.
  std::int64_t                priority;       ///< Priority (lower = more urgent).
  std::optional<std::string>  next_action;    ///< Optional next-action hint.
  std::optional<std::string>  due_at;         ///< Optional due-date/timestamp, stored verbatim.
  std::string                 created_at;     ///< Row creation timestamp.
  std::string                 updated_at;     ///< Row last-update timestamp.
};

/// @brief Arguments to `create`. Mirrors zig's task.zig `CreateArgs`.
export struct task_create_args {
  std::string                 title;                        ///< The task's title (required).
  std::optional<std::string>  body;                         ///< Optional free-text body.
  task_status                 status   = task_status::todo; ///< Initial status.
  std::int64_t                priority = 100;               ///< Priority (lower = more urgent).
  std::optional<std::int64_t> plan_id;                      ///< Owning plan's row id, when set.
  std::optional<std::int64_t> parent_task_id;               ///< Parent task's row id, when set.
  std::optional<std::string>  next_action;                  ///< Optional next-action hint.
  std::optional<std::string>  due_at;                       ///< Optional due-date/timestamp, stored verbatim.
  std::optional<std::string>  slug;                         ///< Optional stable slug.
  bool                        no_auto_promote = false;      ///< Skip plan-304 recompute for this operation.
  std::optional<std::string>  scope;                        ///< Scope slug accepted by `identity::resolve_slug`.
};

/// @brief Arguments to `update`. Mirrors zig's task.zig `UpdateArgs`.
export struct task_update_args {
  std::optional<std::string>  title;                   ///< New title, when set.
  std::optional<std::string>  body;                    ///< New body, when set.
  std::optional<task_status>  status;                  ///< New status, when set (validated via the transition matrix).
  std::optional<std::int64_t> priority;                ///< New priority, when set.
  std::optional<std::int64_t> plan_id;                 ///< New owning plan id, when set.
  std::optional<std::string>  next_action;             ///< New next-action hint, when set.
  std::optional<std::string>  due_at;                  ///< New due-date/timestamp, when set.
  std::optional<std::string>  slug;                    ///< New slug, when set.
  bool                        clear_plan      = false; ///< When true (and `plan_id` unset), clears `plan_id`.
  bool                        no_auto_promote = false; ///< Skip plan-304 recompute for this operation.
  std::optional<std::string>  scope;                   ///< Scope slug accepted by `identity::resolve_slug`.
  /// When true and the status transition is a reopen (from done/cancelled
  /// to todo/doing/blocked), records a `task_reopens` row with
  /// `source='task-update-force'`, and bypasses the transition matrix.
  bool                       force = false;
  std::optional<std::string> reason; ///< Optional reason stored in `task_reopens` when `force` triggers a reopen.
};

/// @brief Filter for `list`. Mirrors zig's task.zig `ListFilter` (minus
/// the multi-scope `scopes` array — same single-`scope` subset as
/// `plan.cppm`'s `plan_list_filter`).
export struct task_list_filter {
  std::optional<task_status>  status;       ///< Match only this status, when set (else the three open statuses).
  std::optional<std::int64_t> plan_id;      ///< Match only tasks with this plan, when set.
  std::optional<std::int64_t> priority_max; ///< Inclusive upper bound on priority, when set.
  std::optional<std::string>  scope;        ///< Scope slug accepted by `identity::resolve_slug`.
};

/// @brief Error surface for every fallible operation in this module.
export enum class task_error : std::uint8_t {
  not_found,
  slug_not_found,
  slug_conflict,
  illegal_transition,
  unknown_status,
  /// A `due_at` that is neither `YYYY-MM-DD` nor a basic RFC3339
  /// timestamp. Landed in task 6135 when `task add` was wired: this file's
  /// header used to record `due_at` validation as NOT ported, on the
  /// grounds that "format enforcement is a `cmd/`-layer input-validation
  /// concern (there is no `--due` flag surface without a `cmd/` binary)".
  /// The `cmd/` binary now exists and declares `--due`, and the oracle
  /// validates in the ENGINE (zig/src/engine/planning/task.zig:323, AFTER
  /// scope resolution at :309) — so the check has to live here to get both
  /// the error and its ORDERING right. Doing it in the handler instead
  /// would report `InvalidDueAt` where the oracle reports `SlugNotFound`
  /// for `--scope nosuchscope --due garbage`.
  invalid_due_at,
  query_failed,
};

/// @brief Create a new task.
/// @param conn An open, migrated database connection.
/// @param args The task's title (required) plus optional fields.
/// @return The created row, or `task_error::slug_conflict` on a slug
/// collision, `task_error::slug_not_found` if `scope` does not resolve
/// (unknown association/repo slug), `task_error::invalid_due_at` if
/// `due_at` is set and is neither `YYYY-MM-DD` nor a basic RFC3339
/// timestamp, or `task_error::query_failed`.
///
/// Note the ORDER: `scope` is resolved BEFORE `due_at` is validated, so
/// an invocation carrying both an unknown scope and a malformed due date
/// reports `slug_not_found`. Mirrors the oracle (:309 then :323).
///
/// When `args.plan_id` is set and `args.no_auto_promote` is false, calls
/// `plan::recompute_status` on that plan after the insert (plan-304).
export auto create_task(db::connection& conn, const task_create_args& args) -> std::expected<task, task_error>;

/// @brief Look up a task by id.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @return The row, or `task_error::not_found`, or `task_error::query_failed`.
export auto show_task(db::connection& conn, std::int64_t id) -> std::expected<task, task_error>;

/// @brief List tasks matching `filter`, ordered by priority then id. When
/// `filter.status` is unset, defaults to the three open statuses
/// (todo/doing/blocked) — mirrors zig's `list` default.
/// @param conn An open, migrated database connection.
/// @param filter Status/plan/priority/scope filters (all optional).
/// @return The matching rows, or `task_error::slug_not_found` if
/// `filter.scope` does not resolve, or `task_error::query_failed`.
export auto list_tasks(db::connection& conn, const task_list_filter& filter) -> std::expected<std::vector<task>, task_error>;

/// @brief Update a task. Builds a dynamic SET clause from whichever
/// fields of `patch` are set; a no-op patch is a successful read-only
/// `show`.
///
/// A `patch.status` change is validated via the task transition matrix
/// before the write (`task_error::illegal_transition` /
/// `task_error::unknown_status`), UNLESS `patch.force` is true, in which
/// case the matrix is bypassed and — when the move is a terminal
/// (done/cancelled) -> open (todo/doing/blocked) escape — a
/// `task_reopens` row is recorded with `source='task-update-force'`.
///
/// When `patch.no_auto_promote` is false, calls `plan::recompute_status`
/// on the task's plan BEFORE the update (its old plan_id, if any) and
/// AFTER (its new plan_id, if changed) — mirrors zig's dual recompute on
/// a plan_id reassignment.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @param patch The fields to change.
/// @return The updated (or unchanged) row, or `task_error::illegal_transition` /
/// `task_error::unknown_status` / `task_error::slug_conflict` / `task_error::not_found` /
/// `task_error::slug_not_found` / `task_error::query_failed`.
export auto update_task(db::connection& conn, std::int64_t id, const task_update_args& patch) -> std::expected<task, task_error>;

/// @brief Mark a task done. Validated via the transition matrix
/// (`force` bypasses it). Recomputes the task's plan status afterward.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @param force When true, bypasses the transition matrix.
/// @return The updated row, or `task_error::illegal_transition` /
/// `task_error::not_found` / `task_error::query_failed`.
export auto mark_done(db::connection& conn, std::int64_t id, bool force) -> std::expected<task, task_error>;

/// @brief Mark a task cancelled. Validated via the transition matrix.
/// Recomputes the task's plan status afterward.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @return The updated row, or `task_error::illegal_transition` /
/// `task_error::not_found` / `task_error::query_failed`.
export auto mark_cancelled(db::connection& conn, std::int64_t id) -> std::expected<task, task_error>;

/// @brief Mark a task blocked on `blocked_on_id`. Validated via the
/// transition matrix. Records an `entity_links` `task -> task`
/// `depends-on` edge. Recomputes the task's plan status afterward.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @param blocked_on_id The task this one is blocked on; must already exist.
/// @param reason Optional human-readable reason, folded into nothing
/// beyond this module (audit summary is not ported — see file header).
/// @param force When true, bypasses the transition matrix.
/// @return The updated row, or `task_error::not_found` (either task id),
/// `task_error::illegal_transition`, or `task_error::query_failed`.
export auto mark_blocked(db::connection& conn, std::int64_t id, std::int64_t blocked_on_id,
                         std::optional<std::string_view> reason, bool force) -> std::expected<task, task_error>;

/// @brief Reopen a done/cancelled task to `new_status`. Bypasses the
/// transition matrix unconditionally (mirrors zig: `reopen` is the
/// verb-gated escape, so it calls the matrix with `force=true`
/// internally). Records a `task_reopens` row with `source='task-reopen'`
/// when the move is FROM a terminal status. Recomputes the task's plan
/// status afterward.
/// @param conn An open, migrated database connection.
/// @param id The task's row id.
/// @param new_status The target status (typically todo/doing/blocked).
/// @param reason Required at the `cmd/` layer in the Zig original;
/// stored verbatim in `task_reopens.reason`.
/// @return The updated row, or `task_error::not_found` / `task_error::query_failed`.
export auto reopen(db::connection& conn, std::int64_t id, task_status new_status, std::string_view reason)
    -> std::expected<task, task_error>;

/// @brief Render one task as the operator-facing key/value block.
///
/// Ports zig/src/engine/planning/task.zig's `renderText` byte for byte.
/// Three details are load-bearing and none of them are cosmetic:
///
///   - Label padding is THIRTEEN columns (`"next action: "`), wider than
///     the plan renderer's nine and the association renderer's ten. Each
///     entity picked its own width in the oracle; there is no shared
///     constant to reach for.
///   - `priority` prints `@max(priority, 0)` — a NEGATIVE priority renders
///     as `0` while the `tasks.priority` column still stores the negative
///     value. Oracle-captured: `task add "neg pri" --priority -5` prints
///     `priority:    0` and the row holds `-5`. Do not "fix" the renderer
///     to print the stored value; a caller that needs the real number
///     reads the row or the JSON, which is NOT clamped.
///   - `scope` is `<kind>` with `:<id>` appended only when `scope_id` is
///     set, so a global task prints a bare `scope:       global`.
///
/// Five lines are conditional (`plan`, `parent`, `next action`, `due`,
/// `body`) and print in exactly that order between `scope` and `created`.
/// `slug` has NO line at all in the oracle's text renderer even though the
/// column exists — it surfaces only in JSON.
/// @param t The task to render.
/// @return The complete block, INCLUDING its trailing newline. The caller
/// writes it verbatim and appends nothing.
export auto render_text(const task& t) -> std::string;

/// @brief Render one task as the single-line JSON object.
///
/// Field order is the `task` struct's declaration order, because the
/// oracle's JSON path is `std.json.Stringify.value` over the Zig `Task`
/// struct and Zig serializes fields in declaration order. Every optional
/// (`scope_id`, `plan_id`, `parent_task_id`, `body`, `slug`,
/// `next_action`, `due_at`) renders as a JSON `null` when unset — NOT as
/// `""`, which is what makes an absent `--body` distinguishable from
/// `--body ""` on the wire as well as in the column.
///
/// `priority` is the STORED value here, unclamped, unlike `render_text`'s
/// `@max(priority, 0)`. Oracle-captured on the same `--priority -5` run.
/// @param t The task to render.
/// @return The JSON object with NO trailing newline — a fragment the
/// caller terminates (the oracle's `output.emit` prints `"\n"` after
/// stringifying).
export auto render_json(const task& t) -> std::string;

} // namespace planar::engine::planning
