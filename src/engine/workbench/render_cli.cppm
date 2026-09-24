/// @file render_cli.cppm
/// @brief `planar.engine.workbench.render_cli` — the operator-facing bytes
/// every `planar workbench *` leaf writes (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of the printing halves of
/// zig/src/cmd/planar/handlers/workbench/*.zig.
///
/// ## Terminator contract — stated per renderer, because it VARIES
///
/// The tree's rule (src/lib/json_text/CMakeLists.txt) is that each renderer
/// declares its own shape, and this module has all three:
///
///   - COMPLETE payloads, terminator included, written verbatim. Every
///     `render_*_json` and `render_*_text` below is one of these. That
///     includes `render_lint_json`, which is NDJSON: the newline is
///     per-RECORD, and an issue-free run is ZERO BYTES rather than a bare
///     newline.
///   - `render_gc_drift_refusal` is likewise complete, but goes to STDERR
///     and is followed by exit 1.
///   - `error_body_*` are FRAGMENTS: a message body with no `error: `
///     prefix and no terminator, which the cmd layer composes.
///
/// Mixing those up silently doubles or drops a newline, which is exactly
/// the class of defect the split exists to prevent.
///
/// ## Why the renderers live at layer 2 rather than in the handlers
///
/// Same reason `engine_local` and `engine_runs` put theirs here: a Catch2
/// case can assert the exact bytes without spawning a process. The handlers
/// become a wiring layer with nothing left to re-derive.
module;

export module planar.engine.workbench.render_cli;

import std;
import planar.engine.workbench.gc;
import planar.engine.workbench.lint;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;

namespace planar::engine::workbench::render_cli {

/// @brief The aggregate `workbench status` (no plan argument) counters.
export struct status_totals {
  std::size_t                       applied   = 0;             ///< Summed across every feature with a tree.
  std::size_t                       pending   = 0;             ///< Summed likewise.
  std::size_t                       conflicts = 0;             ///< Summed likewise.
  std::size_t                       malformed = 0;             ///< Summed likewise.
  std::vector<sync::malformed_file> malformed_files;           ///< Concatenated across features.
  std::size_t                       filtered              = 0; ///< Always 0: `status` never filters.
  std::size_t                       pre_existing_terminal = 0; ///< Always 0, for the same reason.
  std::size_t                       cleaned               = 0; ///< Always 0, for the same reason.
};

/// @brief The one-line-plus-details summary a sync verb prints.
///
/// `push` gets a wider line — it is the only mode that reports `filtered`
/// and the active `(mode=...)` — and is the only mode that can emit the
/// pre-existing-terminal remediation line. VERBOSE suppresses both of
/// those and lists one line per changed entry instead (oracle-confirmed:
/// `push --verbose` on a tree with a pre-existing terminal file prints no
/// remediation line at all).
/// @param plan_id The anchor plan id.
/// @param plan_slug The anchor plan slug.
/// @param run_mode Which verb is running.
/// @param verb The verb's name as it appears in the first line.
/// @param value The run result.
/// @param verbose Whether `--verbose` was passed.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_sync_result_text(std::int64_t plan_id, std::string_view plan_slug, sync::mode run_mode, std::string_view verb,
                                    const sync::result& value, bool verbose) -> std::string;

/// @brief The `--json` payload for a single-plan sync verb.
/// @param value The run result.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_sync_result_json(const sync::result& value) -> std::string;

/// @brief The `--json` payload for `workbench status` with NO plan argument.
///
/// A DIFFERENT shape from the single-plan one: it stops after `cleaned` and
/// carries neither `filter_mode` nor `entries`. Oracle-captured; not an
/// oversight to be tidied.
/// @param totals The aggregated counters.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_status_totals_json(const status_totals& totals) -> std::string;

/// @brief The `workbench list --json` payload. An empty list is `[]`.
/// @param items The features.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_list_json(std::span<const sync::active_feature> items) -> std::string;

/// @brief The `workbench list` text table, or `no features found` when empty.
/// @param items The features.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_list_text(std::span<const sync::active_feature> items) -> std::string;

/// @brief The `workbench gc --json` payload.
/// @param value The summary.
/// @param dry_run Whether `--dry-run` was passed.
/// @param filter_mode The active mode.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_gc_json(const gc::summary& value, bool dry_run, terminal::mode filter_mode) -> std::string;

/// @brief The `workbench gc` text summary.
/// @param value The summary.
/// @param dry_run Whether `--dry-run` was passed.
/// @param filter_mode The active mode.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_gc_text(const gc::summary& value, bool dry_run, terminal::mode filter_mode) -> std::string;

/// @brief The drift refusal `gc` writes to STDERR before exiting 1.
/// @param value The summary carrying `drifted_paths`.
/// @return A COMPLETE stderr payload, terminator included.
export auto render_gc_drift_refusal(const gc::summary& value) -> std::string;

/// @brief The `workbench lint --json` payload: one JSON object per issue,
/// each on its own line. ZERO BYTES when there are no issues.
/// @param issues The diagnostics.
/// @return A COMPLETE stdout payload.
export auto render_lint_json(std::span<const lint::issue> issues) -> std::string;

/// @brief The `workbench lint` text report, ending in the scan tally.
/// @param value The lint result.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_lint_text(const lint::result& value) -> std::string;

/// @brief `workbench archive`'s two payloads.
/// @param plan_id The anchor plan id.
/// @param feature_dir The removed directory.
/// @param json Whether `--json` was passed.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_archive(std::int64_t plan_id, std::string_view feature_dir, bool json) -> std::string;

/// @brief `workbench restore`'s two payloads.
/// @param plan_id The anchor plan id.
/// @param feature_dir The restored directory.
/// @param json Whether `--json` was passed.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_restore(std::int64_t plan_id, std::string_view feature_dir, bool json) -> std::string;

/// @brief `workbench resolve`'s two payloads.
/// @param event_id The settled event.
/// @param prefer Which side was kept.
/// @param json Whether `--json` was passed.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_resolve(std::int64_t event_id, sync::conflict_resolution prefer, bool json) -> std::string;

/// @brief The `no active features found` line `workbench status` prints when
/// no plan has a tree.
/// @return A COMPLETE stdout payload, terminator included.
export auto render_no_active_features() -> std::string;

/// @brief The malformed-file error BODY (`error: ` and the newline are the
/// cmd layer's).
/// @param count How many files were malformed.
/// @param plan_id The plan to name in the remediation, or unset for the
/// `--all` form the no-plan `status` uses.
/// @return The message body.
export auto error_body_malformed(std::size_t count, std::optional<std::int64_t> plan_id) -> std::string;

/// @brief The conflict error BODY.
/// @param count How many conflicts were recorded.
/// @return The message body.
export auto error_body_conflicts(std::size_t count) -> std::string;

/// @brief The lint-issues error BODY.
/// @param errors Error count.
/// @param warnings Warning count.
/// @return The message body.
export auto error_body_lint_issues(std::size_t errors, std::size_t warnings) -> std::string;

} // namespace planar::engine::workbench::render_cli
