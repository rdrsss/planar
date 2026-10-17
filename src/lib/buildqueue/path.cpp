/// @file path.cpp
/// @brief Implementation of `planar.buildqueue.path` (see path.cppm).

module;

module planar.buildqueue.path;

import std;

namespace planar::buildqueue {

namespace {

/// @brief The value of one environment variable, or nullopt when it is absent
/// or empty.
/// @param env The environment lookup.
/// @param name The variable name.
/// @return The non-empty value.
auto usable(const env_lookup& env, std::string_view name) -> std::optional<std::string> {
  auto value = env(name);
  if (!value || value->empty()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

auto error_message(error value) -> std::string {
  switch (value) {
  case error::no_location:
    return std::format("neither {} nor HOME is set, so the build queue database cannot be located; set one of them", k_env_db);
  }
  return std::string{"unknown build queue path error"};
}

auto resolve_queue_db_path(const env_lookup& env) -> std::expected<std::filesystem::path, error> {
  if (auto const explicit_path = usable(env, k_env_db); explicit_path.has_value()) {
    return std::filesystem::path{*explicit_path};
  }
  if (auto const home = usable(env, "HOME"); home.has_value()) {
    return std::filesystem::path{*home} / ".planar" / "build-queue.db";
  }
  return std::unexpected(error::no_location);
}

auto queue_log_directory(const std::filesystem::path& db_path) -> std::filesystem::path {
  auto const stem = db_path.extension() == ".db" ? db_path.stem().string() : db_path.filename().string();
  return db_path.parent_path() / std::format("{}-logs", stem);
}

} // namespace planar::buildqueue
