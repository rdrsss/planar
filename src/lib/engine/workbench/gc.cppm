/// @file gc.cppm
/// @brief `planar.engine.workbench.gc` — remove workbench files whose
/// backing entity has gone terminal (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/gc.zig.
///
/// ## What it does, and what it refuses to do
///
/// Walks a feature tree (or every feature tree under `--all-scopes`), reads
/// each `.md` file's front matter, looks up the backing entity's CURRENT
/// status, and unlinks the file when that status is filtered under the
/// active mode. The verb is FS-first: the only DB write is deleting the
/// file's `workbench_sync_state` row after a successful unlink. No entity
/// row is ever mutated — `gc` cannot cancel a task, unlike `pull`.
///
/// It KEEPS, rather than removes, in three cases that all mean "I do not
/// understand this file, so I will not delete it":
///
///   - the front matter does not parse
///   - the entity kind or status is unrecognized
///   - the entity is not terminal under the active mode
///
/// ## The drift refusal
///
/// `gc` defaults to APPLY (Planar's destructive-verb convention), so the
/// safety valve is drift detection: a file whose current hash differs from
/// the manifest's `content_hash` holds edits that were never pulled, and
/// removing it would silently discard them. Those files are counted in
/// `drifted_skipped` and named in `drifted_paths`; the caller refuses the
/// whole run unless `--yes` was passed. A file with NO manifest row is not
/// drifted — there is nothing to have drifted from.
///
/// ## `--all-scopes` swallows per-plan failures
///
/// A plan whose walk throws is counted in `errors` and the sweep continues,
/// rather than aborting every remaining plan. Carried over deliberately.
module;

export module planar.engine.workbench.gc;

import std;
import planar.db;
import planar.engine.workbench.terminal;

namespace planar::engine::workbench::gc {

/// @brief Run options, mirroring the verb's flags.
export struct options {
  bool           dry_run     = false;                    ///< Count what would be removed; touch nothing.
  bool           yes         = false;                    ///< Remove drifted files anyway.
  terminal::mode filter_mode = terminal::mode::failures; ///< Which terminal statuses qualify.
  bool           all_scopes  = false;                    ///< Informational; the caller picks the entry point.
};

/// @brief What one run did.
export struct summary {
  std::size_t              removed         = 0; ///< Files unlinked (or, under `dry_run`, that would be).
  std::size_t              kept            = 0; ///< Files left alone.
  std::size_t              drifted_skipped = 0; ///< Files held back by the drift refusal.
  std::size_t              errors          = 0; ///< Unreadable files, failed unlinks, failed plan walks.
  std::vector<std::string> drifted_paths;       ///< Absolute paths of the held-back files.
};

/// @brief Failure surface.
export enum class gc_error : std::uint8_t {
  query_failed, ///< SQLite refused an operation.
};

/// @brief Sweep one anchor plan's feature tree.
///
/// A plan with NO tree on disk is a clean no-op, not an error.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param workbench_root The resolved workbench root.
/// @param opts The run options.
/// @return The summary, or the failure.
export auto run_for_plan(db::connection& conn, std::int64_t anchor_plan_id, std::string_view workbench_root, const options& opts)
    -> std::expected<summary, gc_error>;

/// @brief Sweep every top-level plan's feature tree, aggregating one summary.
/// @param conn The database connection.
/// @param workbench_root The resolved workbench root.
/// @param opts The run options.
/// @return The aggregate summary, or the failure.
export auto run_all_scopes(db::connection& conn, std::string_view workbench_root, const options& opts)
    -> std::expected<summary, gc_error>;

} // namespace planar::engine::workbench::gc
