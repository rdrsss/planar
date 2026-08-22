/// @file render.cppm
/// @brief `planar.engine.ingest.render` — text and JSON preview rendering of a
/// proposed diff.
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/render.zig`.
///
/// The JSON projection is the M9 parity gate: its `coverage` object must match
/// the Zig implementation BYTE FOR BYTE against the same input, including the
/// two-space indent, the `", "` separators inside the arrays, and the blank
/// line an empty `entities` array leaves behind. Nothing here is cosmetic —
/// prettifying the output is a contract change.
///
/// Both renderers return an owning `std::string` rather than writing to a
/// stream. The Zig originals take a writer because the engine there is called
/// with the command's buffered stderr/stdout live; returning the bytes keeps
/// this module free of any output dependency (it depends on `db` alone) and
/// makes byte-exactness directly assertable in a unit test rather than only
/// through a binary.

module;

export module planar.engine.ingest.render;

import std;
import planar.engine.ingest.diff;

namespace planar::engine::ingest::render {

/// @brief Renders the tree-shaped human diff preview.
///
/// Always closes with a `coverage:` summary line so scripts have a stable
/// signal, then the slug-collision block when there is one, then either the
/// `Run with --apply …` reminder or `Nothing to do.`.
/// @param d The proposed change set.
/// @param applied `false` (preview) prints the `Run with --apply` reminder;
/// `true` suppresses the footer because the caller is about to print applied
/// statistics instead.
/// @return The rendered text.
export [[nodiscard]] auto render_text(const diff::diff& d, bool applied) -> std::string;

/// @brief Renders the diff as the pretty-printed JSON preview object.
///
/// Schema:
/// @code
/// { anchor_plan_id, assoc_slug, anchor_slug,
///   entities: [{op, kind, title, scope?, derives_from?, touches?}, ...],
///   summary: {additions, updates, removals},
///   coverage: {total_tasks, tasks_with_slug, tasks_without_slug,
///              uncovered_task_slugs: [...], orphan_scenarios: [...]},
///   slug_collisions: [{slug, existing_task_id, existing_plan_id}, ...] }
/// @endcode
/// @param d The proposed change set.
/// @return The rendered JSON, newline-terminated.
export [[nodiscard]] auto render_json(const diff::diff& d) -> std::string;

} // namespace planar::engine::ingest::render
