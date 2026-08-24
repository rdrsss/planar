// @file adapter.t.cpp
// @brief Tests for `planar.adapter` (plan 996, task 6041).
//
// HOME SAFETY. Pure value types and one switch. No file, no socket, no
// environment variable, no path.
//
// ORACLE PROVENANCE. `adapter_error_name` returns Zig error tags, and the
// tags are read off the two Zig error-set declarations directly:
// zig/src/engine/extsync/jira.zig:6-15 and github.zig:8-18 both declare
// `InvalidExternalId, NotFound, UnexpectedStatus, TransportFailed,
// ParseFailed, EncodeFailed, InvalidAuth, WriteFailed`. sync.zig's
// `bestEffortErrorWrite` writes `@errorName(e)` of exactly those into
// `sync_events.detail`, which `audit trail --link <id> --json` renders, so
// the spelling is observable and the assertion below is on the OBSERVABLE
// string, not on a C++ enumerator name.

import std;
import planar.adapter;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("adapter_error_name returns the Zig error tag verbatim", "[adapter]") {
  using planar::adapter::adapter_error;
  using planar::adapter::adapter_error_name;

  CHECK(adapter_error_name(adapter_error::invalid_external_id) == "InvalidExternalId");
  CHECK(adapter_error_name(adapter_error::not_found) == "NotFound");
  CHECK(adapter_error_name(adapter_error::unexpected_status) == "UnexpectedStatus");
  CHECK(adapter_error_name(adapter_error::transport_failed) == "TransportFailed");
  CHECK(adapter_error_name(adapter_error::parse_failed) == "ParseFailed");
  CHECK(adapter_error_name(adapter_error::encode_failed) == "EncodeFailed");
  CHECK(adapter_error_name(adapter_error::invalid_auth) == "InvalidAuth");
  CHECK(adapter_error_name(adapter_error::write_failed) == "WriteFailed");
}

TEST_CASE("field_change_set distinguishes unrequested from empty", "[adapter]") {
  // Not a tautology about std::optional: this is the contract every adapter's
  // push() relies on to decide whether to emit a field at all. A change set
  // carrying an EMPTY title is a request to clear the remote title; one
  // carrying no title is a request to leave it alone.
  planar::adapter::field_change_set const untouched;
  CHECK_FALSE(untouched.title.has_value());

  planar::adapter::field_change_set const cleared{.title = std::string{}};
  REQUIRE(cleared.title.has_value());
  CHECK(cleared.title->empty());
}

TEST_CASE("remote_state keeps the provider status alongside the mapped one", "[adapter]") {
  // Both fields exist because the sync engine compares `status` (Planar's
  // vocabulary) while operator-facing surfaces show `raw_status`. Collapsing
  // them would silently make an unmappable provider status compare equal to
  // "no status".
  planar::adapter::remote_state const state{.status = "doing", .raw_status = "In Progress"};
  CHECK(state.status == "doing");
  CHECK(state.raw_status == "In Progress");
  CHECK(state.version.empty());
}
