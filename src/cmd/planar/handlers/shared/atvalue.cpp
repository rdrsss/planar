/// @file atvalue.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.atvalue`.

module planar.cmd.planar.handlers.atvalue;

import std;
import planar.engine.planning;
import planar.cmd.planar.exit;

namespace planar::cmd::handlers {

auto resolve_at_value(std::optional<std::string> raw, std::string_view label)
    -> std::expected<std::optional<std::string>, domain_error> {
  if (!raw.has_value()) {
    return std::optional<std::string>{};
  }
  auto read = engine::planning::read_body(*raw);
  if (!read) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("read {}: FileNotFound", label)));
  }
  return std::optional<std::string>{std::move(*read)};
}

} // namespace planar::cmd::handlers
