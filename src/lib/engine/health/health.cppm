/// @file health.cppm
/// @brief `planar.engine.health` — the lifecycle-hygiene reporter behind
/// `planar health hygiene` (plan 996, task 6090).
///
/// Behavior-preserving port (D2) of zig/src/engine/health.zig's `hygiene`
/// and `renderHygieneText`.
///
/// ## What is NOT here, and why the bare `planar health` leaf stays unported
///
/// `health.zig` also carries `check` / `withProjectionFreshness` — the
/// database + handoff + INSTALLED-PROJECTION report behind the bare
/// `planar health` verb. `check` alone would port easily; the leaf as a
/// whole would not, because its handler folds
/// `engine.installedsurface.status` into every run, and
/// zig/src/engine/installedsurface.zig is 548 lines of manifest-driven
/// filesystem classification with no counterpart anywhere in this tree.
/// Porting `check` without it would produce a `planar health` whose
/// `projection_freshness` block is a permanent `not_installed` stub and
/// whose `overall` rollup — the field the leaf's documented exit-1-on-
/// degraded contract reads — would disagree with the oracle on any machine
/// with a managed install. That is a leaf that compiles and is wrong, so
/// `health` is deferred WITH its dependency and only `health hygiene`
/// lands here. Named rather than dropped silently.
///
/// ## `hygiene` has NO cwd-derived read set, unlike its sibling listings
///
/// A null `scope` reports across EVERY scope in the database rather than
/// narrowing to the caller's cwd. That is deliberate in the oracle and it
/// is what makes the verb useful — hygiene drift is a whole-database
/// question — but it is the opposite of what `plan list` / `search` do
/// with the same absent flag, so it is stated here rather than left to be
/// inferred. When `scope` IS given it must name an ASSOCIATION:
/// `repo:<slug>` and `global` both refuse.
module;

export module planar.engine.health;

import std;
import planar.db;

namespace planar::engine::health {

/// @brief Per-status task tally attached to a stale draft plan.
export struct task_counts {
  std::int64_t todo      = 0; ///< Tasks in `todo`.
  std::int64_t doing     = 0; ///< Tasks in `doing`.
  std::int64_t blocked   = 0; ///< Tasks in `blocked`.
  std::int64_t done      = 0; ///< Tasks in `done`.
  std::int64_t cancelled = 0; ///< Tasks in `cancelled`.
};

/// @brief A draft plan that is either an empty wrapper or has only
/// terminal tasks.
export struct stale_draft_plan {
  std::int64_t                id = 0;         ///< The `plans` row id.
  std::optional<std::int64_t> parent_plan_id; ///< The parent plan, when nested.
  std::string                 title;          ///< The plan title.
  /// @brief `"zero_tasks"` when no task was ever attached, else
  /// `"all_tasks_terminal"`. Two literals, not an enum, because they are
  /// the JSON wire values.
  std::string reason;
  task_counts counts;     ///< The per-status tally.
  std::string suggestion; ///< The repair command, reported but never run.
};

/// @brief A task left in `doing` past the threshold.
export struct stale_doing_task {
  std::int64_t id      = 0;  ///< The `tasks` row id.
  std::int64_t plan_id = 0;  ///< The owning plan.
  std::string  scope;        ///< `global` / `assoc:<slug>` / `repo:<slug>`.
  std::string  title;        ///< The task title.
  std::int64_t age_days = 0; ///< Whole days since `updated_at`, truncated.
  std::string  suggestion;   ///< The repair command, reported but never run.
};

/// @brief A question left `open` past the threshold.
export struct stale_open_question {
  std::int64_t id = 0;       ///< The `questions` row id.
  std::string  title;        ///< The question title.
  std::int64_t age_days = 0; ///< Whole days since `updated_at`, truncated.
  std::string  suggestion;   ///< The repair command, reported but never run.
};

/// @brief The thresholds echoed back in the report.
export struct hygiene_thresholds {
  std::int64_t stale_doing_days = 7;  ///< Doing-task age cutoff.
  std::int64_t stale_open_days  = 30; ///< Open-question age cutoff.
};

/// @brief The whole read-only lifecycle-drift report.
///
/// Member order is the JSON key order the oracle emits.
export struct hygiene_report {
  hygiene_thresholds               thresholds;           ///< The thresholds used.
  std::vector<stale_draft_plan>    stale_draft_plans;    ///< Ordered by plan id.
  std::vector<stale_doing_task>    stale_doing_tasks;    ///< Ordered by task id.
  std::vector<stale_open_question> stale_open_questions; ///< Ordered by question id.
};

/// @brief Inputs to `hygiene`.
export struct hygiene_options {
  /// @brief Limit findings to one ASSOCIATION slug. Unset reports every
  /// scope — see this file's header.
  std::optional<std::string> scope;
  std::int64_t               stale_doing_days = 7;  ///< Doing-task cutoff, in days. Negative refuses.
  std::int64_t               stale_open_days  = 30; ///< Open-question cutoff, in days. Negative refuses.
  /// @brief A fixed clock for deterministic tests, as an ISO-8601 string
  /// SQLite's `julianday` accepts. Unset means `'now'`.
  ///
  /// This exists in the oracle for the same reason and is the ONLY way to
  /// test the age boundaries without sleeping: the thresholds are compared
  /// against `julianday(coalesce(?, 'now'))`, so a test pins the left-hand
  /// side rather than the row timestamps.
  std::optional<std::string> now;
};

/// @brief Error surface for `hygiene`.
export enum class hygiene_error : std::uint8_t {
  invalid_threshold, ///< A threshold was negative.
  unsupported_scope, ///< `scope` resolved to something other than an association.
  slug_not_found,    ///< `scope` resolved to no row at all.
  query_failed,      ///< An underlying SQL statement failed.
};

/// @brief Collect plan, task, and question lifecycle drift. Pure read.
///
/// The three sections are independent queries with independent rules:
///
///   - A DRAFT plan is stale when it has zero tasks (`reason =
///     "zero_tasks"`) or when every task it has is `done`/`cancelled`
///     (`reason = "all_tasks_terminal"`). No age threshold applies —
///     staleness here is structural, not temporal.
///   - A `doing` task is stale when `julianday(now) - julianday(updated_at)`
///     is STRICTLY GREATER than `stale_doing_days`. Strictly: a task
///     exactly `stale_doing_days` old does not appear.
///   - An `open` question is stale under the same strict comparison
///     against `stale_open_days`.
///
/// `age_days` is a `cast(... as integer)` of that same difference, which
/// TRUNCATES toward zero rather than rounding.
/// @param conn An open, migrated database connection.
/// @param options Scope, thresholds, and the optional fixed clock.
/// @return The report, or a `hygiene_error`.
export auto hygiene(db::connection& conn, const hygiene_options& options) -> std::expected<hygiene_report, hygiene_error>;

/// @brief Render a hygiene report the way `planar health hygiene` prints
/// it without `--json`.
///
/// Three sections, always all three, each with a `  none` line when its
/// list is empty — an empty report is six lines of structure, not zero
/// bytes.
/// @param report The report to render.
/// @return The complete stdout payload INCLUDING its trailing newline.
export auto render_hygiene_text(const hygiene_report& report) -> std::string;

} // namespace planar::engine::health
