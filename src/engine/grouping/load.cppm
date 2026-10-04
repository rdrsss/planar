/// @file load.cppm
/// @brief `planar.engine.grouping.load` — the DB-facing bridge between a
/// plan's persisted state and the pure greedy partitioner, plus the two
/// `groups recommend` renderers (plan 996, task 6095).
///
/// Behavior-preserving port (D2) of zig/src/engine/grouping/load.zig plus the
/// rendering half of zig/src/cmd/planar/handlers/groups/recommend.zig. The
/// leaf is READ-ONLY: this module only SELECTs.
///
/// It loads three things and hands them to `greedy::group`:
///
///   1. The plan's OPEN (`status = 'todo'`) tasks, ordered by priority then
///      id — the same candidate selection `recommend-strategy` uses, so the
///      grouping arm and the eligibility arm agree on what is in play.
///      Oracle-confirmed exclusion: a `done` task with a closure does not
///      appear and does not contribute to any cost.
///   2. Each task's EFFECTIVE closure from `closures`: rows with role
///      `modify` or `reference`. `transitive` rows are EXCLUDED. Confirmed
///      rather than assumed — the fixture's task 3 carries a `transitive`
///      symbol of weight 999 and its slice came back at cost 53, not 1052.
///   3. The dependency DAG from `entity_links` `depends-on` edges, restricted
///      to edges whose BOTH endpoints are open tasks of this plan.
///
/// ## Edge orientation — load-bearing, and easy to invert
///
/// `entity_links(from_id, to_id, 'depends-on')` means **from_id depends on
/// to_id**. `greedy::dep` is the opposite orientation (`blocker` precedes
/// `blocked`), so the mapping is:
///
///     Dep{ .blocked = from_id, .blocker = to_id }
///
/// Inverting it would let the cycle check reason about a mirrored DAG and
/// emit an unschedulable grouping. There is a dedicated test that asserts
/// each produced field by value for exactly this reason.
///
/// ## What is NOT ported
///
/// - **The `mtkahypar` solver arm.** Master ships greedy only (decision
///   1293). The `--solver` FLAG is fully modelled here, including its refusal
///   message and every reporting field of the JSON envelope, and
///   `recommend_with` runs over the abstract `optimal::arm` seam
///   (`optimal_arm.cppm`). Master passes `optimal::none()`, so
///   `--solver mtkahypar` degrades to greedy with `"optimal_available":false`.
///   The solver implementation lives on branch `dev/grouping-solvers`.
/// - **`policy.audit` rows** — no such module in the C++ tree, and this leaf
///   is read-only.

module;

export module planar.engine.grouping.load;

import std;
import planar.db;
import planar.engine.grouping.greedy;
import planar.engine.grouping.optimal_arm;

namespace planar::engine::grouping::load {

/// @brief Which partitioner the operator asked for.
export enum class solver {
  greedy,   ///< The always-available heuristic. The default.
  mtkahypar ///< The optional external hypergraph solver (a name for the optimal arm; none ships on master).
};

/// @brief Parse a `--solver` value.
/// @param text The flag value.
/// @return The solver, or `std::nullopt` for anything else (the caller then
/// refuses at exit 2).
export auto solver_from_text(std::string_view text) -> std::optional<solver>;

/// @brief Render a solver as it appears in both output forms.
/// @param s The solver.
/// @return `"greedy"` or `"mtkahypar"`.
export auto solver_to_text(solver s) -> std::string_view;

/// @brief The default per-slice window budget when `--budget` is absent.
///
/// Oracle-captured as the literal `128000` in the emitted envelope, not
/// merely documented in help text.
export inline constexpr std::uint32_t k_default_budget = 128000;

/// @brief A plan's grouping recommendation.
export struct recommendation {
  std::int64_t     plan_id    = 0;                ///< The plan grouped.
  std::uint32_t    budget     = k_default_budget; ///< Budget it was computed under.
  std::size_t      open_tasks = 0;                ///< Open (todo) tasks considered.
  greedy::grouping grouping_;                     ///< The formed slices.
  solver           solver_ = solver::greedy;      ///< The solver that ACTUALLY ran.
  /// True iff the optimal arm ran and produced a partition. Always false
  /// from `recommend`; `recommend_with` sets it when the supplied arm ran.
  bool optimal_available = false;
  /// True iff the optimal arm ran but greedy's lower-cost result shipped. It
  /// is also false under plain degradation, so this flag never distinguishes
  /// "no arm" from "the arm failed".
  bool selected_greedy = false;
};

/// @brief Failure surface for this module.
export enum class grouping_error {
  not_found,   ///< No plan with that id.
  query_failed ///< Any SQLite failure.
};

/// @brief Load a plan's open tasks, their effective closures, and the
/// dependency DAG, then group them under `budget` via the greedy arm.
/// @param conn An open, migrated database connection.
/// @param plan_id The plan to group.
/// @param budget The per-slice window budget.
/// @return The recommendation, or `grouping_error::not_found` when the plan
/// does not exist. A plan that exists with no open tasks is a SUCCESS with
/// zero slices, not a not_found.
export auto recommend(db::connection& conn, std::int64_t plan_id, std::uint32_t budget)
    -> std::expected<recommendation, grouping_error>;

/// @brief Solver-aware grouping recommendation (task 6460).
///
/// `requested == solver::greedy` is identical to `recommend`. For
/// `solver::mtkahypar`: when the arm is unavailable (master ships none, or
/// the arm's call itself fails), this degrades SILENTLY to the plain greedy
/// result with `optimal_available:false` — the exact behavior a genuinely
/// solver-less machine already produces, so a caller cannot distinguish "not
/// built" from "not installed".
///
/// When the arm IS available, both arms run on the identical loaded inputs
/// and are scored with the SAME `greedy::grouping::total_cost()`. The lower
/// (or tied) grouping ships; `solver` reports `mtkahypar` and
/// `optimal_available` is `true` in both outcomes (the optimal arm ran
/// either way), while `selected_greedy` records whether greedy's own result
/// was the one that shipped. This is the "never worse than greedy" contract
/// (task 4247): the returned grouping's cost is never higher than greedy's.
/// @param conn An open, migrated database connection.
/// @param plan_id The plan to group.
/// @param budget The per-slice window budget.
/// @param requested Which solver the caller asked for.
/// @param optimal_arm The optimal arm to run for `solver::mtkahypar`. Master passes
/// `optimal::none()`.
/// @return The recommendation, or `grouping_error::not_found` when the plan
/// does not exist.
export auto recommend_with(db::connection& conn, std::int64_t plan_id, std::uint32_t budget, solver requested,
                           const optimal::arm& optimal_arm) -> std::expected<recommendation, grouping_error>;

/// @brief The plan's open (todo) task ids, ordered by priority then id.
/// @param conn An open, migrated database connection.
/// @param plan_id The plan.
/// @return The candidate task ids.
export auto load_open_task_ids(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::vector<std::int64_t>, grouping_error>;

/// @brief A task's effective-closure units from `closures`.
///
/// `transitive` rows are excluded. A symbol listed under BOTH roles folds to a
/// SINGLE unit with role `modify` (modify dominates, so the write-conflict
/// detection sees it) and the larger of the two weights.
/// @param conn An open, migrated database connection.
/// @param task_id The task.
/// @return Its units, one per distinct symbol.
export auto load_units(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<greedy::unit>, grouping_error>;

/// @brief The dependency edges among a set of open tasks.
///
/// Only edges with BOTH endpoints in `open_ids` are returned — an edge to a
/// done or foreign task does not constrain how the open tasks group.
/// Self-edges are dropped. See this module's header for the orientation flip.
/// @param conn An open, migrated database connection.
/// @param open_ids The open-task set to restrict to.
/// @return The edges, oriented as `greedy::dep` expects.
export auto load_deps(db::connection& conn, std::span<const std::int64_t> open_ids)
    -> std::expected<std::vector<greedy::dep>, grouping_error>;

/// @brief Render `groups recommend --json`.
/// @param rec The recommendation.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_json(const recommendation& rec) -> std::string;

/// @brief Render `groups recommend`'s text form.
///
/// A header line of `key:value` pairs separated by TWO spaces, then one block
/// per slice. The zero-slice case prints a parenthesised note instead of an
/// empty list.
/// @param rec The recommendation.
/// @return The text block, WITH a trailing newline.
export auto render_text(const recommendation& rec) -> std::string;

/// @brief The `plan <id> not found` message this leaf emits at exit 1.
///
/// Note the id is NOT quoted here, unlike `test-spec status`'s
/// `plan '<arg>' not found` — the two leaves genuinely differ.
/// @param plan_id The plan id that did not resolve.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_plan_not_found(std::int64_t plan_id) -> std::string;

/// @brief The refusal for a non-integer plan positional.
/// @param argument The rejected positional, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_plan_id(std::string_view argument) -> std::string;

/// @brief The refusal for a `--budget` value that is not a non-negative
/// integer.
/// @param argument The rejected value, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_budget(std::string_view argument) -> std::string;

/// @brief The refusal for an unknown `--solver` value.
///
/// Note this one carries NO leaf prefix — captured as a bare
/// `error: --solver must be 'greedy' or 'mtkahypar', got 'bogus'`.
/// @param argument The rejected value, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_solver(std::string_view argument) -> std::string;

} // namespace planar::engine::grouping::load
