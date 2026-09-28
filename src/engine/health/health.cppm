/// @file health.cppm
/// @brief `planar.engine.health` — the database/handoff/installed-
/// projection reporter behind `planar health` and `planar health hygiene`
/// (plan 996, tasks 6090 and 6357).
///
/// Behavior-preserving port (D2) of zig/src/engine/health.zig's `hygiene`,
/// `renderHygieneText`, `check`, `withProjectionFreshness` and `renderText`.
///
/// ## `check` / `withProjectionFreshness` and the layer-1 extraction that
/// unblocked them
///
/// Task 6090 landed only `hygiene`/`render_hygiene_text` here, because the
/// bare `planar health` leaf's handler folds
/// `engine.installedsurface.status` into every run, and
/// zig/src/engine/installedsurface.zig (548 lines of manifest-driven
/// filesystem classification) had no counterpart anywhere in this tree. A
/// `check` ported without it would report a permanently-stubbed
/// `projection_freshness` and get `overall` — the field the leaf's
/// documented exit-1-on-degraded contract reads — wrong on any machine with
/// a managed install.
///
/// Task 6357 closed that gap the same way task 6352 closed `report`'s
/// (decision 981): rather than a direct `engine_health -> engine_<classifier>`
/// edge (the `engine_* -> engine_*` shape D15/D18 FATAL on), the classifier
/// was ported straight to LAYER 1 as `planar.installed_surface` — it holds
/// no `db` handle, so there was no edge pulling it toward `engine_health` in
/// the first place. `check` returns a permanently-stubbed
/// `projection_freshness` on its own (matching the oracle's `check`, which
/// never touches `installedsurface`); `with_projection_freshness` is the
/// separate fold-in step the CLI handler (layer 3) composes, exactly
/// mirroring the oracle's two-call shape.
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
import planar.installed_surface;

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

/// @brief The `check`/`with_projection_freshness` half's stale-handoff
/// cutoff, in hours. 24 mirrors the oracle's default.
export inline constexpr std::int64_t stale_handoff_threshold_hours = 24;

/// @brief The installed-projection section of `report`.
///
/// Member order is the JSON key order the oracle emits.
export struct projection_freshness {
  std::string                       state; ///< `not_installed` / `fresh` / `degraded`.
  installed_surface::manifest_state manifest_status =
      installed_surface::manifest_state::missing; ///< Mirrors the classifier's manifest state.
  std::size_t managed            = 0;             ///< `fresh + stale + missing`.
  std::size_t fresh              = 0;             ///< Managed rows that match the staged authority.
  std::size_t stale              = 0;             ///< Managed rows that disagree with the staged authority.
  std::size_t missing            = 0;             ///< Managed rows whose installed path is absent.
  std::size_t unmanaged          = 0;             ///< Destination entries with no manifest row.
  std::size_t unselected_vendors = installed_surface::supported_vendors.size(); ///< Vendors the manifest never names.
  std::optional<std::string> evidence;       ///< The classifier's own reason string, when it has one.
  std::optional<std::string> repair_command; ///< Set only when `degraded`; the full reinstall command.
};

/// @brief The whole `planar health` snapshot.
///
/// Member order is the JSON key order the oracle emits — `output.emit`
/// stringifies this struct with `std.json`'s DEFAULT options, so every
/// optional member inside `projection_freshness` is emitted as JSON `null`
/// when unset (never omitted); see `health_hygiene`'s `render_json` for the
/// same convention on `parent_plan_id`.
export struct report {
  std::string                  db_path;                     ///< The database path, echoed back verbatim.
  bool                         db_ok               = true;  ///< Whether the connection itself is usable.
  std::int64_t                 schema_version      = 0;     ///< The highest applied migration version.
  std::int64_t                 schema_target       = 0;     ///< The highest EMBEDDED migration version.
  bool                         schema_current      = false; ///< `schema_version == schema_target`.
  std::int64_t                 migration_count     = 0;     ///< Rows in `schema_migrations`.
  bool                         integrity_ok        = false; ///< `PRAGMA integrity_check` answered `ok`.
  std::int64_t                 inflight_tasks      = 0;     ///< Tasks in `doing`/`blocked`.
  std::int64_t                 resumable_tasks     = 0;     ///< In-flight tasks with a `next_action` and a snapshot.
  std::int64_t                 not_resumable_tasks = 0;     ///< `inflight_tasks - resumable_tasks`.
  std::int64_t                 pending_handoffs    = 0;     ///< Handoffs in `pending`/`validated`.
  std::int64_t                 stale_handoffs      = 0;     ///< Pending handoffs older than `stale_handoff_threshold_hours`.
  health::projection_freshness projection_freshness;        ///< The installed-projection contributor.
  std::string                  overall;                     ///< `"ok"` or `"degraded"`.
};

/// @brief Why `check` refused outright.
export enum class check_error : std::uint8_t {
  schema_table_missing, ///< The FIRST query (`max(version) from schema_migrations`) failed — table absent, or any other failure
                        ///< of that specific query; the oracle's `catch return Error.SchemaTableMissing` does not distinguish.
  query_failed, ///< The SECOND query (`count(*) from schema_migrations`) failed, having run right after the first succeeded.
};

/// @brief Snapshot database + handoff health. Pure read: issues single-row
/// queries and assembles the struct, no IO beyond the connection, no
/// formatting.
///
/// `projection_freshness` here is ALWAYS the permanent `not_installed` stub
/// — the oracle's `check` never touches `installedsurface` either; folding
/// in the real classification is `with_projection_freshness`'s job, called
/// separately by the CLI handler.
/// @param conn An open, migrated database connection.
/// @param db_path Echoed into the report's `db_path` field verbatim.
/// @return The report, or why the snapshot could not be taken.
export auto check(db::connection& conn, std::string_view db_path) -> std::expected<report, check_error>;

/// @brief Fold an `installed_surface::status_result` into an existing
/// database health report as its projection-freshness contributor.
///
/// A manifest state of `legacy`/`invalid`/`unsupported`, OR any managed row
/// classified `stale`/`missing`, degrades BOTH `projection_freshness.state`
/// and (if not already degraded) `report.overall`. A `fresh` classification
/// with zero managed drift never un-degrades an already-degraded report —
/// `overall` only ever moves toward `"degraded"`, never back.
/// @param report_in The base report, typically from `check`.
/// @param status The classifier's result.
/// @return The report with `projection_freshness` (and possibly `overall`) updated.
export auto with_projection_freshness(report report_in, const installed_surface::status_result& status) -> report;

/// @brief Render `report` the way `planar health` prints it without
/// `--json`. Mirrors the oracle's `renderText` label set.
/// @param report The report to render.
/// @return The complete stdout payload INCLUDING its trailing newline.
export auto render_text(const report& report) -> std::string;

/// @brief The wire spelling of a `manifest_state` — its Zig enum tag name,
/// which `std.json.Stringify` emits verbatim for an enum field and which
/// `render_text`'s `projection manifest:` line also uses.
/// @param value The manifest state.
/// @return Its lowercase name, e.g. `"unsupported"`.
export auto manifest_state_name(installed_surface::manifest_state value) -> std::string_view;

} // namespace planar::engine::health
