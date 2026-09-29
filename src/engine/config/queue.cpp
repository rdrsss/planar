/// @file queue.cpp
/// @brief Implementation of `planar.engine.config.queue` (see queue.cppm).
module planar.engine.config.queue;

import std;
import planar.engine.config.toml;

namespace planar::engine::config {

auto default_queue_settings() -> queue_settings {
  return {};
}

auto validate_queue(const toml_map& doc) -> std::vector<queue_finding> {
  static_cast<void>(doc);
  return {};
}

auto queue_from_map(const toml_map& doc) -> std::expected<queue_settings, std::vector<queue_finding>> {
  static_cast<void>(doc);
  return queue_settings{};
}

auto load_queue_settings(const std::filesystem::path& config_path) -> std::expected<queue_settings, queue_load_error> {
  static_cast<void>(config_path);
  return queue_settings{};
}

} // namespace planar::engine::config
