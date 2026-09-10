/// @file adapter.cpp
/// @brief Implementation of `planar.adapter`. See adapter.cppm for the
/// layering argument and why `adapter_error_name` is a parity surface.

module planar.adapter;

import std;

namespace planar::adapter {

external_adapter::~external_adapter() = default;

auto adapter_error_name(adapter_error err) -> std::string_view {
  // These are Zig error tags, verbatim. They reach the operator through
  // `sync_events.detail` (written by sync.zig's failure path as
  // `@errorName(e)`) and out via `audit trail --link <id> --json`.
  switch (err) {
  case adapter_error::invalid_external_id:
    return "InvalidExternalId";
  case adapter_error::not_found:
    return "NotFound";
  case adapter_error::unexpected_status:
    return "UnexpectedStatus";
  case adapter_error::transport_failed:
    return "TransportFailed";
  case adapter_error::parse_failed:
    return "ParseFailed";
  case adapter_error::encode_failed:
    return "EncodeFailed";
  case adapter_error::invalid_auth:
    return "InvalidAuth";
  case adapter_error::write_failed:
    return "WriteFailed";
  }
  return "UnexpectedStatus";
}

} // namespace planar::adapter
