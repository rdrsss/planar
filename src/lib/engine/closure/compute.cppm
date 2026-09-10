/// @file compute.cppm
/// @brief `planar.engine.closure.compute` — tree-sitter backed derived-
/// closure extraction (task 6189).
module;
#include <tree_sitter/api.h>
export module planar.engine.closure.compute;
import std;
import planar.db;
namespace planar::engine::closure::compute {

/// @brief Row-count summary of one `extract` call, echoed back to the
/// caller alongside the persisted `store::row`s themselves.
export struct result {
  std::int64_t task_id{};      ///< The task the closure was computed for.
  std::size_t  seeds{};        ///< Touched `(repo, path)` seed pairs walked.
  std::size_t  modify{};       ///< Rows classified `role = "modify"`.
  std::size_t  reference{};    ///< Rows classified `role = "reference"`.
  std::size_t  transitive{};   ///< Rows classified `role = "transitive"`.
  std::size_t  rows_written{}; ///< Total rows actually persisted to `store`.
};

/// @brief Failure surface for `extract`.
export enum class error {
  no_seeds,    ///< The task has no `task_touch_paths` rows to seed from.
  query_failed ///< Any SQLite failure while reading seeds or writing rows.
};

/// @brief The extractor version stamped on every `store::row` this module
/// writes (`store::row::extractor_version`), bumped whenever the tree-sitter
/// extraction rules change so stale rows can be told apart from fresh ones.
export inline constexpr std::string_view extractor_version = "m2-closure-0.1";

/// @brief The real, module-internal (non-exported) implementation. Defined
/// in compute.cpp, where the tree-sitter grammar walk actually lives.
/// @param conn Open database connection.
/// @param task_id The task whose touched paths seed the walk.
/// @return See `extract`, which returns this call's result unchanged.
auto extract_impl(db::connection& conn, std::int64_t task_id) -> std::expected<result, error>;

/// @brief Walk `task_id`'s `task_touch_paths` seeds with tree-sitter,
/// extract modify/reference/transitive rows, and persist them via `store`.
/// @param conn Open database connection.
/// @param task_id The task whose touched paths seed the walk.
/// @return The row-count `result` summary on success, `error::no_seeds` when
/// the task has no `task_touch_paths` rows, or `error::query_failed` for any
/// SQLite failure while reading seeds or writing rows.
///
/// Deliberately a thin, fully INLINE wrapper over `extract_impl` rather than
/// an ordinary exported declaration whose definition lives in compute.cpp:
/// doxygen 1.18.0's C++20-modules parser cannot associate a doc comment with
/// a declaration split across a `.cppm`/`.cpp` pair for this particular
/// function — confirmed by isolated repro (identical doc comment, over the
/// declaration alone with the body in compute.cpp: fails; collapsed into one
/// inline definition here: passes), independent of and in addition to the
/// name-collision issue this function's `extract_impl` -> `extract` split
/// also worked around (`run` collided with three unrelated
/// `planar::cmd::*::run` functions elsewhere in the tree; matching precedent
/// in `core/version.cppm`, which is also a single-file inline export).
/// `extract_impl` carries no doc comment of its own: it is not exported, and
/// Doxyfile.lint's `EXTRACT_ALL = NO` does not require one.
export auto extract(db::connection& conn, std::int64_t task_id) -> std::expected<result, error> {
  return extract_impl(conn, task_id);
}

} // namespace planar::engine::closure::compute
