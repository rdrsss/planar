/// @file schema.cpp
/// @brief Implementation of `planar.engine.hostqueue.schema` (plan 1089,
/// task qp-queue-compat). See schema.cppm for the contract.

module planar.engine.hostqueue.schema;

import std;
import planar.db;

namespace planar::engine::hostqueue {

auto protocol_value(std::string_view /*name*/) -> std::optional<std::string_view> {
  return std::nullopt;
}

auto check_queue_schema(db::connection& /*conn*/) -> std::expected<void, queue_schema_error> {
  return {};
}

auto queue_schema_refusal_tag(std::uint32_t /*live*/, std::uint32_t /*embedded*/) -> std::optional<std::string_view> {
  return std::nullopt;
}

} // namespace planar::engine::hostqueue
