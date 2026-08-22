// @file exit.t.cpp
// @brief Unit tests for `planar.cli.exit` (task cpp-cli-output-logging).
//
// Oracle captures (task brief: derive expected values by running the
// reference binary, never hand-assumed):
//
//   $ ./zig/zig-out/bin/planar task bogus; echo $?
//   2
//   $ ./zig/zig-out/bin/planar-agent fail --reason x; echo $?   # missing --claim
//   1
#include <catch2/catch_test_macros.hpp>

import std;
import planar.cli.exit;
import planar.cli.error;

using planar::cli::binary_kind;
using planar::cli::domain_error_kind;
using planar::cli::exit_code_for;

TEST_CASE("exit_code_for: planar maps parse_error to exit 2 (user input)", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::parse_error, binary_kind::planar) == planar::cli::exit_user_input);
  REQUIRE(exit_code_for(domain_error_kind::parse_error, binary_kind::planar) == 2);
}

// task 6063: planar-agent's exit.zig header documents 2 but its codeFor
// switch has no Parse arm, so parse errors fall through to the generic-1
// bucket. This port reproduces the ACTUAL (not the documented) behavior.
TEST_CASE("exit_code_for: planar_agent falls through to exit 1 for parse_error (task 6063)", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::parse_error, binary_kind::planar_agent) == planar::cli::exit_generic_failure);
  REQUIRE(exit_code_for(domain_error_kind::parse_error, binary_kind::planar_agent) == 1);
}

TEST_CASE("exit_code_for: not_found folds into the generic-1 bucket for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::not_found, binary_kind::planar) == 1);
  REQUIRE(exit_code_for(domain_error_kind::not_found, binary_kind::planar_agent) == 1);
}

TEST_CASE("exit_code_for: invalid_input/invalid_entity_ref map to exit 2 for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::invalid_input, binary_kind::planar) == 2);
  REQUIRE(exit_code_for(domain_error_kind::invalid_input, binary_kind::planar_agent) == 2);
  REQUIRE(exit_code_for(domain_error_kind::invalid_entity_ref, binary_kind::planar) == 2);
  REQUIRE(exit_code_for(domain_error_kind::invalid_entity_ref, binary_kind::planar_agent) == 2);
}

TEST_CASE("exit_code_for: sync_conflict maps to exit 3 for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::sync_conflict, binary_kind::planar) == 3);
  REQUIRE(exit_code_for(domain_error_kind::sync_conflict, binary_kind::planar_agent) == 3);
}

TEST_CASE("exit_code_for: scope_mismatch maps to exit 5 for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::scope_mismatch, binary_kind::planar) == 5);
  REQUIRE(exit_code_for(domain_error_kind::scope_mismatch, binary_kind::planar_agent) == 5);
}

TEST_CASE("exit_code_for: slug_conflict/already_exists map to exit 6 for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::slug_conflict, binary_kind::planar) == 6);
  REQUIRE(exit_code_for(domain_error_kind::already_exists, binary_kind::planar_agent) == 6);
}

TEST_CASE("exit_code_for: schema_version_ahead maps to exit 7 for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::schema_version_ahead, binary_kind::planar) == 7);
  REQUIRE(exit_code_for(domain_error_kind::schema_version_ahead, binary_kind::planar_agent) == 7);
}

// Pre-M3 trap 2 (M2 boundary review, plan 996 task 6066): zig/src/runtime/
// runtime.zig:103's SchemaVersionBehind guard is a THIRD per-binary
// divergence, independent of parse_error/task 6063 above.
// zig/src/cmd/planar-agent/exit.zig and zig/src/cmd/planar-watch/exit.zig
// both fold it into the SAME exit 7 as SchemaVersionAhead;
// zig/src/cmd/planar/exit.zig has NO SchemaVersionBehind arm at all and
// falls through to the generic exit 1. Break-probe: schema_version_ahead
// and schema_version_behind must NOT collapse to the same code for
// `binary_kind::planar` (7 vs 1) even though they DO for `planar_agent`
// (7 == 7) — asserting only the agent side would pass a regression that
// silently added a SchemaVersionBehind => 7 arm to the planar binary too.
TEST_CASE("exit_code_for: schema_version_behind diverges per binary — planar falls through to 1, planar_agent maps to 7",
          "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::schema_version_behind, binary_kind::planar) == 1);
  REQUIRE(exit_code_for(domain_error_kind::schema_version_behind, binary_kind::planar_agent) == 7);
  // Non-vacuous cross-check: for `planar`, schema_version_ahead (7) and
  // schema_version_behind (1) must differ from each other.
  REQUIRE(exit_code_for(domain_error_kind::schema_version_ahead, binary_kind::planar) !=
          exit_code_for(domain_error_kind::schema_version_behind, binary_kind::planar));
  // For planar_agent, the two schema-version kinds DO collapse to the same
  // code (7) — matching the Zig source's shared switch arm exactly.
  REQUIRE(exit_code_for(domain_error_kind::schema_version_ahead, binary_kind::planar_agent) ==
          exit_code_for(domain_error_kind::schema_version_behind, binary_kind::planar_agent));
}

TEST_CASE("exit_code_for: not_implemented maps to exit 64 for both binaries", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::not_implemented, binary_kind::planar) == 64);
  REQUIRE(exit_code_for(domain_error_kind::not_implemented, binary_kind::planar_agent) == 64);
}

// Break-probe: generic_failure (the truly unmapped-error bucket) must
// itself resolve to exit 1, not accidentally inherit exit_success (0) —
// a default-initialized enum or an off-by-one in the switch's default arm
// would silently report success for a real failure.
TEST_CASE("exit_code_for: generic_failure never resolves to exit_success", "[cli][exit]") {
  REQUIRE(exit_code_for(domain_error_kind::generic_failure, binary_kind::planar) != planar::cli::exit_success);
  REQUIRE(exit_code_for(domain_error_kind::generic_failure, binary_kind::planar) == 1);
}
