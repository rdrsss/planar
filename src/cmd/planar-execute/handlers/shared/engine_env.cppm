/// @file engine_env.cppm
/// @brief Read the process engine override at command dispatch.
module;
#include <cstdlib>
export module planar.cmd.planar_execute.handlers.shared.engine_env;
import std;

namespace planar::cmd::execute::handlers::shared {
/// @brief Provide the engine env command operation.
/// @return Computed value.
export auto engine_env() -> std::optional<std::string_view> {
  char const* raw = std::getenv("PLANAR_EXECUTE_ENGINE"); // NOLINT(concurrency-mt-unsafe) — single-threaded startup.
  return raw == nullptr ? std::nullopt : std::optional<std::string_view>{raw};
}
} // namespace planar::cmd::execute::handlers::shared
