/// Tree-sitter backed derived-closure extraction (task 6189).
module;
#include <tree_sitter/api.h>
export module planar.engine.closure.compute;
import std;
import planar.db;
export namespace planar::engine::closure::compute {
struct result {
  std::int64_t task_id{};
  std::size_t  seeds{}, modify{}, reference{}, transitive{}, rows_written{};
};
enum class error { no_seeds, query_failed };
inline constexpr std::string_view extractor_version = "m2-closure-0.1";
auto                              run(db::connection&, std::int64_t) -> std::expected<result, error>;
} // namespace planar::engine::closure::compute
