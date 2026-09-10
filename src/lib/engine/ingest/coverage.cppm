/// @file coverage.cppm
/// @brief `planar.engine.ingest.coverage` — the `--strict` gate's coverage
/// object.
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/coverage.zig`.
///
/// Coverage measures how many tasks in a diff are verified by at least one
/// test-spec scenario. Slug-form `**Verifies:** task:<slug>` is the citation
/// chain — NUMERIC refs are ignored for coverage purposes, because at preview
/// time a numeric id cannot be tied to a roadmap bullet and at apply time the
/// task may not exist yet.
///
/// The gate is informational by default; `--strict` promotes uncovered tasks
/// and orphan scenarios from a printed warning into a non-zero exit. The JSON
/// projection of this object is the M9 parity gate, so two ordering rules are
/// contractual and must not be "tidied":
///
///   * `uncovered_task_slugs_` is DEDUPLICATED and sorted bytewise. Two tasks
///     cannot share a slug (the index forbids it), but the dedupe is what the
///     original does and removing it would change the count under a
///     hand-built diff.
///   * `orphan_scenarios_` is sorted bytewise but NOT deduplicated. Two
///     distinct scenarios may legitimately carry the same title, and
///     collapsing them would under-report the gap.

module;

export module planar.engine.ingest.coverage;

import std;
import planar.engine.ingest.diff;

namespace planar::engine::ingest::coverage {

/// @brief Test-spec coverage of the tasks in one diff.
export struct coverage {
  std::size_t              total_tasks_        = 0; ///< Tasks proposed as add or update.
  std::size_t              tasks_with_slug_    = 0; ///< Tasks carrying a `[slug:]`, and so citable.
  std::size_t              tasks_without_slug_ = 0; ///< Tasks with no slug; nothing can cite them.
  std::vector<std::string> uncovered_task_slugs_;   ///< Deduped, sorted bytewise.
  std::vector<std::string> orphan_scenarios_;       ///< Titles of scenarios citing nothing; sorted, NOT deduped.

  /// @brief Whether the strict gate should refuse.
  /// @return `true` when there is at least one uncovered task or orphan scenario.
  [[nodiscard]] auto has_gaps() const -> bool;
};

/// @brief Computes coverage over a proposed diff.
///
/// Pure: reads only the diff, touches no database.
/// @param d The proposed change set.
/// @return The coverage object.
export [[nodiscard]] auto compute(const diff::diff& d) -> coverage;

} // namespace planar::engine::ingest::coverage
