// @file exit_codes.t.cpp
// @brief The COMPLETE exit-code table for the `planar` OPERATOR binary,
// pinned row by row (plan 996, task 6123).
//
// ## Why this file exists, and why there are three of them
//
// Until task 6123 the numbers lived once, in layer-1's
// `planar.cli.exit::exit_code_for(domain_error_kind, binary_kind)`, and
// each binary's own `exit` module was a one-line binding to its row. That
// shared, binary-PARAMETERIZED helper is the exact hazard task 6066 spent a
// rename fighting: overload resolution picked between a binary-aware and a
// binary-blind `exit_code_for` purely by ARGUMENT COUNT, so a call site
// written one token short silently applied the operator binary's policy
// from an agent code path — a real bug waiting for dispatch wiring to trip
// over it, not a hypothetical.
//
// Task 6123 deleted `src/lib/cli` entirely, and its brief was explicit
// about not reintroducing a shared parameterized helper. So the table now
// lives once PER BINARY, complete and local, in
// `src/cmd/<binary>/exit.cppm` — and this file is the other half of that
// arrangement: with no single shared table to test, each binary pins its
// own, in a file compiled into that binary's test target alone.
//
// Two rows genuinely diverge across the three binaries, and BOTH are
// asserted here from this binary's side and again from the other two:
//
//     parse_error            2 HERE, 1 on planar-agent and planar-watch.
//     schema_version_behind  1 HERE, 7 on planar-agent and planar-watch.
//
// `zig/src/cmd/planar/exit.zig`'s `codeFor` has NO `SchemaVersionBehind`
// arm — only `SchemaVersionAhead => 7` — so it falls through to the generic
// `else => 1` bucket, where both other binaries fold the two together at 7.
// Reproduced, not "fixed" (D2).
//
// Also pinned here, and moved DOWN-STREAM rather than dropped: the
// `scope_mismatch -> 5` assertion that used to live in
// src/lib/engine/identity/scope.t.cpp against the shared layer-1 table. An
// engine test may not import a `cmd_*` module (an upward layer-2 ->
// layer-3 edge; cmake/architecture.cmake FATALs on it), so it lands here,
// in all three binaries, which is strictly more coverage than the two lines
// it replaces.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cmd.planar.exit;

using planar::cmd::domain_error_kind;
using planar::cmd::error_from_body;
using planar::cmd::error_from_rendered;
using planar::cmd::exit_code;
using planar::cmd::exit_code_for;
using planar::cmd::report;

TEST_CASE("planar: every domain-error kind maps to its documented exit code", "[cmd][exit][exitcode]") {
  // Asserted as a COMPLETE table, in one case, on purpose. Exit-code
  // mapping is the easiest thing in this tree to test vacuously: a table
  // collapsed to a single value passes any test that checks one bucket.
  CHECK(exit_code_for(domain_error_kind::generic_failure) == 1);
  CHECK(exit_code_for(domain_error_kind::not_found) == 1);
  CHECK(exit_code_for(domain_error_kind::invalid_input) == 2);
  CHECK(exit_code_for(domain_error_kind::invalid_entity_ref) == 2);
  CHECK(exit_code_for(domain_error_kind::parse_error) == 2);
  CHECK(exit_code_for(domain_error_kind::sync_conflict) == 3);
  CHECK(exit_code_for(domain_error_kind::scope_mismatch) == 5);
  CHECK(exit_code_for(domain_error_kind::slug_conflict) == 6);
  CHECK(exit_code_for(domain_error_kind::already_exists) == 6);
  CHECK(exit_code_for(domain_error_kind::schema_version_ahead) == 7);
  CHECK(exit_code_for(domain_error_kind::schema_version_behind) == 1);
  CHECK(exit_code_for(domain_error_kind::not_implemented) == 64);
}

TEST_CASE("planar: the table is not collapsed — every distinct code is reachable", "[cmd][exit][exitcode]") {
  // The break-probe the case above cannot be on its own: a table that
  // returned a constant would satisfy any single row. This asserts the
  // SHAPE of the whole mapping — seven distinct values out of twelve kinds
  // — so a collapse fails here even if someone "fixed" the rows above to
  // match.
  std::set<int> const codes{
      exit_code_for(domain_error_kind::generic_failure),       exit_code_for(domain_error_kind::not_found),
      exit_code_for(domain_error_kind::invalid_input),         exit_code_for(domain_error_kind::invalid_entity_ref),
      exit_code_for(domain_error_kind::parse_error),           exit_code_for(domain_error_kind::sync_conflict),
      exit_code_for(domain_error_kind::scope_mismatch),        exit_code_for(domain_error_kind::slug_conflict),
      exit_code_for(domain_error_kind::already_exists),        exit_code_for(domain_error_kind::schema_version_ahead),
      exit_code_for(domain_error_kind::schema_version_behind), exit_code_for(domain_error_kind::not_implemented),
  };
  CHECK(codes == std::set<int>{1, 2, 3, 5, 6, 7, 64});
}

TEST_CASE("planar: the two per-binary divergences, from this binary's side", "[cmd][exit][exitcode][divergence]") {
  // THE OPERATOR BINARY'S SIDE of the pair the other two invert. If this
  // module ever went back to a shared, binary-parameterized helper, the
  // single most likely symptom is one of these two flipping to the other
  // binary's value — which is why they are asserted again, separately from
  // the complete table above, with the counterpart named.
  //
  // planar-agent and planar-watch: parse_error -> 1, schema_version_behind -> 7.
  CHECK(exit_code_for(domain_error_kind::parse_error) == 2);
  CHECK(exit_code_for(domain_error_kind::schema_version_behind) == 1);
  CHECK(exit_code_for(domain_error_kind::parse_error) != exit_code_for(domain_error_kind::generic_failure));
  CHECK(exit_code_for(domain_error_kind::schema_version_behind) != exit_code_for(domain_error_kind::schema_version_ahead));
}

TEST_CASE("planar: exit_code(domain_error) agrees with exit_code_for(kind)", "[cmd][exit][exitcode]") {
  // The envelope overload is what dispatch actually calls; a divergence
  // between the two would make every assertion above describe a function
  // nothing runs.
  for (auto const kind :
       {domain_error_kind::generic_failure, domain_error_kind::not_found, domain_error_kind::invalid_input,
        domain_error_kind::invalid_entity_ref, domain_error_kind::parse_error, domain_error_kind::sync_conflict,
        domain_error_kind::scope_mismatch, domain_error_kind::slug_conflict, domain_error_kind::already_exists,
        domain_error_kind::schema_version_ahead, domain_error_kind::schema_version_behind, domain_error_kind::not_implemented}) {
    CHECK(exit_code(error_from_body(kind, "x")) == exit_code_for(kind));
    CHECK(exit_code(error_from_rendered(kind, "x")) == exit_code_for(kind));
  }
}

TEST_CASE("planar: report composes a body but writes a rendered payload verbatim", "[cmd][exit]") {
  // The two stderr shapes. Getting this wrong doubles the `error: ` prefix
  // on every renderer-sourced failure.
  std::ostringstream body_out;
  report(error_from_body(domain_error_kind::invalid_input, "--anchor-path is required"), body_out);
  CHECK(body_out.str() == "error: --anchor-path is required\n");

  std::ostringstream rendered_out;
  report(error_from_rendered(domain_error_kind::generic_failure, "error: workflow 'nope' not found\n"), rendered_out);
  CHECK(rendered_out.str() == "error: workflow 'nope' not found\n");
}
