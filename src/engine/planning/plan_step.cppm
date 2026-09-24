/// @file plan_step.cppm
/// @brief `planar.engine.planning.plan_step` — the `plan_steps` entity:
/// ordered action items belonging to a plan (plan 996, task 6187).
///
/// Behavior-preserving port (D2) of zig/src/engine/planning/plan_step.zig,
/// landed together with the five `plan step` leaves that call it. Until
/// this task those leaves refused at exit 64 because the entity had no
/// port at all — `planning.cppm`'s umbrella said so explicitly.
///
/// ## The ordinal contract, and why `--after` is a misnomer
///
/// `add` assigns `max(ordinal) + 1` for the plan when no ordinal is
/// supplied (1 on an empty plan). When one IS supplied it is used
/// VERBATIM and nothing is renumbered, so a collision with an existing
/// step surfaces `ordinal_conflict` from the `unique (plan_id, ordinal)`
/// constraint rather than shifting later steps down.
///
/// The CLI spells that argument `--after`, which reads as "insert after
/// step N" and is NOT what happens — `--after 1` on a plan whose steps are
/// at ordinals 1 and 2 sets ordinal 1 and CONFLICTS. Oracle-derived by
/// running it, not read off the flag name:
///
///   $Z plan step add 1 inserted --after 1   exit 1
///       stderr b'error: ordinal conflict: a step at that position already exists\n'
///   $Z plan step add 1 'explicit five' --after 5 --json   exit 0  ordinal 5
///
/// The engine parameter is therefore named `ordinal`, matching what it
/// does; the flag keeps its oracle spelling because the surface is pinned.
///
/// ## Transitions
///
/// `{pending, in_progress}` -> `done`; `{pending}` -> `skipped`. `done` and
/// `skipped` are terminal and refuse every further move. The two refusals
/// share one engine error (`invalid_transition`) and the CALLER phrases
/// them differently — the oracle says `step N is already terminal` for
/// `done` and `step N cannot be skipped (must be pending)` for `skip`,
/// including when `skip` is applied to an already-terminal step. Both exit
/// 1, so only a byte assertion distinguishes them; `plan_task_remainder_
/// leaves.t.cpp` carries one for each.
///
/// ## Status spelling
///
/// The in-flight status is `in_progress` in C++ and `in-progress` in the
/// database (migration 1's CHECK constraint) — `status_to_text` emits the
/// hyphenated form and `status_from_text` accepts only it. A port that let
/// the underscore form reach the INSERT would be rejected by SQLite, not
/// silently stored.
module;

export module planar.engine.planning.plan_step;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief The `plan_steps.status` domain. Mirrors migration 1's CHECK
/// constraint: any other value is rejected by SQLite itself.
export enum class step_status : std::uint8_t {
  pending,     ///< Not started. The only status `skip` accepts.
  in_progress, ///< Started. Stored as the HYPHENATED `in-progress`.
  done,        ///< Terminal: completed.
  skipped,     ///< Terminal: deliberately not done.
};

/// @brief Parse the database spelling of a step status.
/// @param s The stored text (`pending` / `in-progress` / `done` / `skipped`).
/// @return The status, or `std::nullopt` when `s` is not one of the four.
export auto step_status_from_text(std::string_view s) -> std::optional<step_status>;

/// @brief The exact text stored in `plan_steps.status`.
/// @param s The status.
/// @return The spelling the CHECK constraint accepts — note `in-progress`.
export auto step_status_to_text(step_status s) -> std::string_view;

/// @brief Whether `s` admits no further transition.
/// @param s The status.
/// @return `true` for `done` and `skipped`.
export auto step_status_is_terminal(step_status s) -> bool;

/// @brief One row of the `plan_steps` table.
export struct plan_step {
  std::int64_t                id;         ///< Primary key.
  std::int64_t                plan_id;    ///< Owning plan; `on delete cascade`.
  std::int64_t                ordinal;    ///< Position within the plan; unique per plan.
  std::string                 body;       ///< The step text. `not null`.
  step_status                 status;     ///< Lifecycle status.
  std::optional<std::int64_t> task_id;    ///< Materializing task, or unset. Genuinely nullable.
  std::string                 created_at; ///< ISO-8601 UTC creation stamp.
  std::string                 updated_at; ///< ISO-8601 UTC last-write stamp.
};

/// @brief Arguments for `add_step`.
export struct plan_step_add_args {
  std::int64_t                plan_id; ///< The plan to append to.
  std::string                 body;    ///< The step text.
  std::optional<std::int64_t> ordinal; ///< Explicit position; when unset, `max + 1`. See the file header on `--after`.
};

/// @brief Error surface for every fallible operation in this module.
export enum class plan_step_error : std::uint8_t {
  not_found,          ///< No such plan (on `add`), or no such step / task.
  invalid_transition, ///< The step is terminal, or `skip` was applied to a non-`pending` step.
  ordinal_conflict,   ///< `unique (plan_id, ordinal)` rejected the INSERT.
  query_failed,       ///< SQLite refused the statement.
  audit_write_failed, ///< The `audit_log` row could not be written.
};

/// @brief Append a step to a plan.
///
/// Verifies the plan exists FIRST, so a step is never orphaned by a typo'd
/// plan id — the `plan_id` foreign key would catch it, but as
/// `query_failed` rather than the oracle's `no plan with id N`.
/// @param conn An open, migrated database connection.
/// @param args The plan, the body, and an optional explicit ordinal.
/// @return The created row; `not_found` when the plan does not exist;
/// `ordinal_conflict` when an explicit ordinal is already taken.
export auto add_step(db::connection& conn, const plan_step_add_args& args) -> std::expected<plan_step, plan_step_error>;

/// @brief Look up one step by its primary key.
/// @param conn An open, migrated database connection.
/// @param id The step's row id.
/// @return The row, or `not_found`, or `query_failed`.
export auto show_step(db::connection& conn, std::int64_t id) -> std::expected<plan_step, plan_step_error>;

/// @brief Every step of a plan, ordered by ordinal ascending.
///
/// Does NOT verify the plan exists: an unknown plan id yields an EMPTY
/// list, not `not_found`. That is the oracle's behaviour (`plan step list
/// 999` exits 0 and prints `no steps for plan 999`) and is reproduced
/// rather than tightened under D2 — the `NotFound` arm the Zig handler
/// carries for this call is dead code there, and adding a real existence
/// check here would change an operator-visible exit code.
/// @param conn An open, migrated database connection.
/// @param plan_id The owning plan's row id.
/// @return The rows in ordinal order (possibly empty), or `query_failed`.
export auto list_steps(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<plan_step>, plan_step_error>;

/// @brief Transition a step to `done`. Valid from `pending` and `in_progress`.
/// @param conn An open, migrated database connection.
/// @param id The step's row id.
/// @return The updated row; `not_found`; or `invalid_transition` when the
/// step is already terminal.
export auto mark_step_done(db::connection& conn, std::int64_t id) -> std::expected<plan_step, plan_step_error>;

/// @brief Transition a step to `skipped`. Valid from `pending` ONLY.
/// @param conn An open, migrated database connection.
/// @param id The step's row id.
/// @return The updated row; `not_found`; or `invalid_transition` when the
/// step is terminal OR merely `in_progress`.
export auto skip_step(db::connection& conn, std::int64_t id) -> std::expected<plan_step, plan_step_error>;

/// @brief Point a step at the task that materializes it.
///
/// Both the step and the task must exist; the oracle reports either
/// absence with the SAME message (`step N or task M not found`), so this
/// returns one `not_found` for both rather than distinguishing them.
/// @param conn An open, migrated database connection.
/// @param step_id The step's row id.
/// @param task_id The task's row id.
/// @return The updated row, `not_found`, or `query_failed`.
export auto link_step_task(db::connection& conn, std::int64_t step_id, std::int64_t task_id)
    -> std::expected<plan_step, plan_step_error>;

/// @brief Clear a step's task link (sets `task_id` to SQL NULL).
///
/// No CLI leaf reaches this today — `plan step` has no `unlink` verb. It
/// is ported because it is the other half of `link_step_task`'s write and
/// the only way to restore the NULL state, and because leaving it out
/// would make the `task_id` column one-way from the engine's side.
/// @param conn An open, migrated database connection.
/// @param step_id The step's row id.
/// @return The updated row, `not_found`, or `query_failed`.
export auto unlink_step_task(db::connection& conn, std::int64_t step_id) -> std::expected<plan_step, plan_step_error>;

/// @brief Render one step as the oracle's `plan step add` / `done` / `skip`
/// / `link` text block.
///
/// The `task_id` line is OMITTED entirely when the link is unset — it is
/// not rendered as an empty value — which is what makes the NULL and the
/// zero cases distinguishable on stdout.
/// @param s The step.
/// @return The complete payload INCLUDING its trailing newline (the
/// module's stdout-terminator contract).
export auto render_step_text(const plan_step& s) -> std::string;

/// @brief Render one step as the oracle's single-step JSON object.
/// @param s The step.
/// @param with_ok Whether to lead with `"ok":true`. The mutation leaves
/// (`add`, `done`, `skip`, `link`) emit it; the objects nested inside
/// `plan step list`'s array do NOT. Same engine, two shapes — see
/// `render_step_list_json`.
/// @return The object with NO trailing newline (a fragment; the caller
/// composes it).
export auto render_step_json(const plan_step& s, bool with_ok) -> std::string;

/// @brief Render `plan step list`'s text table.
/// @param steps The steps, in ordinal order.
/// @param plan_id The plan, needed for the empty-case message.
/// @return The complete payload including its trailing newline.
export auto render_step_list_text(std::span<const plan_step> steps, std::int64_t plan_id) -> std::string;

/// @brief Render `plan step list`'s JSON array.
/// @param steps The steps, in ordinal order.
/// @return The array with NO trailing newline (a fragment).
export auto render_step_list_json(std::span<const plan_step> steps) -> std::string;

} // namespace planar::engine::planning
