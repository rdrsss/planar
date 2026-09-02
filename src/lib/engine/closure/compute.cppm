/// Tree-sitter backed derived-closure extraction (task 6189).
module;
#include <tree_sitter/api.h>
export module planar.engine.closure.compute;
import std;
import planar.db;
export namespace planar::engine::closure::compute {

/// @brief Row-count summary of one `run` extraction, echoed back to the
/// caller alongside the persisted `store::row`s themselves.
struct result {
  std::int64_t task_id{};      ///< The task the closure was computed for.
  std::size_t  seeds{};        ///< Touched `(repo, path)` seed pairs walked.
  std::size_t  modify{};       ///< Rows classified `role = "modify"`.
  std::size_t  reference{};    ///< Rows classified `role = "reference"`.
  std::size_t  transitive{};   ///< Rows classified `role = "transitive"`.
  std::size_t  rows_written{}; ///< Total rows actually persisted to `store`.
};

/// @brief Failure surface for `run`.
enum class error {
  no_seeds,    ///< The task has no `task_touch_paths` rows to seed from.
  query_failed ///< Any SQLite failure while reading seeds or writing rows.
};
inline constexpr std::string_view extractor_version = "m2-closure-0.1";
auto                              run(db::connection&, std::int64_t) -> std::expected<result, error>;
} // namespace planar::engine::closure::compute
