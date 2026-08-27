// @file routing.t.cpp
// @brief Tests for `planar.engine.workspace.routing` (plan 996, task 6110).
//
// ============================================================================
// WHAT IS UNDER TEST, AND WHAT DELIBERATELY IS NOT
// ============================================================================
// The READ half of the routing table: decode, the two render arms, and the
// missing-file refusal. The BUILDER stays deferred (see this bucket's
// CMakeLists), so nothing here constructs a table from a database — every
// fixture is routing-table.json BYTES, which is exactly the interface
// `workspace routing show` actually consumes.
//
// This module touches no filesystem and no database: `decode` takes a
// string_view and both renderers return strings. So unlike doctor.t.cpp
// beside it, there is no HOME-safety apparatus here, because there is no
// destination to escape to. The handler that reads the file is where path
// resolution lives, and it is covered in src/cmd/planar/workspace_routing_
// show_leaf.t.cpp against a scratch tree.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Every expected byte string below was captured by RUNNING
// zig/zig-out/bin/planar in a pinned scratch arena, with BOTH streams routed
// through a pipe (never a file redirect — zig's writer does positional writes,
// so a second write to a shared capture file lands at offset 0 and eats the
// first). Fixture: HOME / PLANAR_HOME / PLANAR_DB all under a scratch root, an
// org registered via `workspace init --no-scan --name Acme --slug acme`, and
// routing-table.json overwritten per probe.
//
// --- the --json arm never parses -------------------------------------------
//   file = b'this is not json'   (no trailing newline)
//   $Z workspace routing show --json   exit 0, stdout b'this is not json\n'
//
//   file = b''                    (empty)
//   $Z workspace routing show --json   exit 0, stdout b'\n'
//
// --- but the TEXT arm does, and its two failures differ in EXIT CODE --------
//   file = b'this is not json'
//   $Z workspace routing show   exit 1,
//     stderr b'error: decoding routing table failed: SyntaxError\n'
//
//   file = valid JSON, `schema_version` removed
//   $Z workspace routing show   exit 2,
//     stderr b'error: decoding routing table failed: InvalidInput\n'
//
//   file = valid JSON, a project with no `planar_focus`
//   $Z workspace routing show   exit 2, same InvalidInput message
//
// --- generator_version defaults --------------------------------------------
//   file = valid, `generator_version` absent
//   $Z workspace routing show   exit 0, stdout
//     b'workspace: org:zz (id 7)\ngenerated: GEN (static-v1)\nprojects:  0\n\n'
//
// --- the full text render --------------------------------------------------
//   Two projects: `alpha` with capabilities + depends_on + an EMPTY summary,
//   `beta` with no capabilities + a summary + no deps; one cross-repo edge.
//   $Z workspace routing show   exit 0, stdout
//     b'workspace: org:acme (id 3)\ngenerated: 2020-01-01T00:00:00Z (gv-9)\n'
//     b'projects:  2\n\n'
//     b'- alpha\n    path:         /r/alpha\n'
//     b'    capabilities: go-service, rust\n    summary:      (no summary)\n'
//     b'    depends_on:   beta\n    open tasks:   4\n    open Qs:      5\n'
//     b'- beta\n    path:         /r/beta\n'
//     b'    capabilities: (none)\n    summary:      Beta repo.\n'
//     b'    open tasks:   0\n    open Qs:      0\n'
//     b'\ncross-repo edges:\n  alpha -> beta (go.mod replace)\n'
//
//   Note `alpha` prints `(no summary)` while carrying `summary_source` of
//   `""`, and `beta` omits `depends_on` entirely. Those are the two shapes a
//   plausible re-implementation gets wrong.
//
// --- the missing-file refusal ----------------------------------------------
//   $Z workspace routing show   exit 1, stderr
//     b'error: routing table not found at <path>; run `planar workspace
//       routing build` first\n'
//   `workspace regenerate`'s equivalent OMITS the path; they are NOT the same
//   string.

import std;
import planar.engine.workspace;
import planar.json_dom;

#include <catch2/catch_test_macros.hpp>

namespace routing = planar::engine::workspace::routing;

namespace {

/// @brief A minimal well-formed table, as a format string with one `{}` hole
/// for extra top-level members.
///
/// Deliberately assembled from literal text rather than from the module's own
/// constants: a fixture derived from the code under test would agree with it
/// by construction. `static-v1` below is typed out for the same reason, NOT
/// read from `routing::generator_version_static`.
constexpr std::string_view k_minimal = R"({"schema_version":1,"workspace_id":7,)"
                                       R"("workspace_slug":"zz","workspace_name":"ZZ",)"
                                       R"("generated_at":"GEN","projects":[],)"
                                       R"("cross_repo":{"plans_scoped_to_org":[],)"
                                       R"("questions_scoped_to_org":[],"dependency_edges":[]}})";

/// @brief The two-project fixture whose render the oracle captured.
constexpr std::string_view k_full =
    R"({"schema_version":1,"workspace_id":3,"workspace_slug":"acme","workspace_name":"Acme",)"
    R"("generated_at":"2020-01-01T00:00:00Z","generator_version":"gv-9","projects":[)"
    R"({"slug":"alpha","root_path":"/r/alpha","git_remote":"","summary":"","summary_source":"",)"
    R"("capabilities":["go-service","rust"],"capabilities_source":"static",)"
    R"("depends_on":["beta"],"depends_on_source":"go.mod","entry_points":["go.mod"],)"
    R"("languages":{"go":0.9},)"
    R"("planar_focus":{"active_plans":[1,2],"open_tasks":4,"open_questions":5,"recent_session_ids":[]}},)"
    R"({"slug":"beta","root_path":"/r/beta","git_remote":"","summary":"Beta repo.",)"
    R"("summary_source":"readme","capabilities":[],"capabilities_source":"",)"
    R"("depends_on":[],"depends_on_source":"","entry_points":[],"languages":{},)"
    R"("planar_focus":{"active_plans":[],"open_tasks":0,"open_questions":0,"recent_session_ids":[]}}],)"
    R"("cross_repo":{"plans_scoped_to_org":[9],"questions_scoped_to_org":[],)"
    R"("dependency_edges":[{"from":"alpha","to":"beta","reason":"go.mod replace"}]}})";

} // namespace

// ---------------------------------------------------------------------------
// the --json arm
// ---------------------------------------------------------------------------

TEST_CASE("the json arm emits the file's bytes verbatim, parsing nothing", "[engine][workspace][routing][show]") {
  // The whole point: bytes that are not JSON at all still come back out.
  CHECK(routing::show_json("this is not json") == "this is not json\n");
  // And the decoder AGREES that they are not JSON — proving the json arm
  // really does bypass it rather than the bytes happening to be valid.
  auto decoded = routing::decode("this is not json");
  REQUIRE_FALSE(decoded.has_value());
  CHECK(decoded.error() == routing::decode_error::syntax);
}

TEST_CASE("the json arm appends a newline only when one is missing", "[engine][workspace][routing][show]") {
  CHECK(routing::show_json("") == "\n");           // empty file -> a lone newline
  CHECK(routing::show_json("{}") == "{}\n");       // no terminator -> one added
  CHECK(routing::show_json("{}\n") == "{}\n");     // already terminated -> unchanged
  CHECK(routing::show_json("{}\n\n") == "{}\n\n"); // and NOT normalised to one
}

// ---------------------------------------------------------------------------
// decode: the two failure kinds, kept apart because their exit codes differ
// ---------------------------------------------------------------------------

TEST_CASE("malformed bytes and a missing required field are DIFFERENT failures", "[engine][workspace][routing][decode]") {
  auto syntax = routing::decode("this is not json");
  REQUIRE_FALSE(syntax.has_value());
  CHECK(syntax.error() == routing::decode_error::syntax);
  CHECK(routing::decode_error_name(syntax.error()) == "SyntaxError");

  // Well-formed JSON, but `schema_version` is gone.
  auto invalid = routing::decode(R"({"workspace_id":1,"workspace_slug":"a","workspace_name":"A",)"
                                 R"("generated_at":"T","projects":[],)"
                                 R"("cross_repo":{"dependency_edges":[]}})");
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error() == routing::decode_error::invalid);
  CHECK(routing::decode_error_name(invalid.error()) == "InvalidInput");

  // The distinction is the point — collapsing them would move an exit code.
  CHECK(syntax.error() != invalid.error());
}

TEST_CASE("the three PARSE failures carry three different oracle tags", "[engine][workspace][routing][decode]") {
  // All oracle-captured. They share one exit code (1) and differ only in
  // the interpolated tag, so nothing but a per-input assertion catches a
  // port that collapses them — which an earlier draft of this module did.
  struct probe {
    std::string_view      input;
    routing::decode_error expected;
    std::string_view      tag;
  };
  const std::array<probe, 11> cases{{
      // ran OUT of input
      {"", routing::decode_error::end_of_input, "UnexpectedEndOfInput"},
      {"   ", routing::decode_error::end_of_input, "UnexpectedEndOfInput"},
      {"{", routing::decode_error::end_of_input, "UnexpectedEndOfInput"},
      {R"({"a":)", routing::decode_error::end_of_input, "UnexpectedEndOfInput"},
      {R"({"a":"b)", routing::decode_error::end_of_input, "UnexpectedEndOfInput"},
      // choked on a byte, with input REMAINING
      {"this is not json", routing::decode_error::syntax, "SyntaxError"},
      {"x{}", routing::decode_error::syntax, "SyntaxError"},
      {"{} trailing", routing::decode_error::syntax, "SyntaxError"},
      {R"({"a":1,})", routing::decode_error::syntax, "SyntaxError"},
      {R"({"a":NaN})", routing::decode_error::syntax, "SyntaxError"},
      // repeated a key
      {R"({"schema_version":1,"schema_version":2})", routing::decode_error::duplicate_field, "DuplicateField"},
  }};
  for (const auto& item : cases) {
    INFO("input: " << item.input);
    auto decoded = routing::decode(item.input);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == item.expected);
    CHECK(routing::decode_error_name(decoded.error()) == item.tag);
  }
}

TEST_CASE("a top-level non-object PARSES and then fails as InvalidInput", "[engine][workspace][routing][decode]") {
  // The distinction that is easy to get backwards: these are well-formed
  // JSON, so they are NOT syntax errors — they reach the object check and
  // exit 2 rather than 1. Captured for all five.
  for (std::string_view input : {"[]", "42", R"("hi")", "null", "true"}) {
    INFO("input: " << input);
    auto decoded = routing::decode(input);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == routing::decode_error::invalid);
    CHECK(routing::decode_error_name(decoded.error()) == "InvalidInput");
  }
}

TEST_CASE("every required top-level field fails the decode when absent", "[engine][workspace][routing][decode]") {
  // The PRESENT case first, so the absence assertions below cannot pass
  // merely because the fixture was broken all along.
  REQUIRE(routing::decode(k_minimal).has_value());

  struct removal {
    std::string_view key;
    std::string_view without; // k_minimal with that member deleted
  };
  const std::array<removal, 6> cases{{
      {"schema_version",
       R"({"workspace_id":7,"workspace_slug":"zz","workspace_name":"ZZ","generated_at":"GEN","projects":[],"cross_repo":{"dependency_edges":[]}})"},
      {"workspace_id",
       R"({"schema_version":1,"workspace_slug":"zz","workspace_name":"ZZ","generated_at":"GEN","projects":[],"cross_repo":{"dependency_edges":[]}})"},
      {"workspace_slug",
       R"({"schema_version":1,"workspace_id":7,"workspace_name":"ZZ","generated_at":"GEN","projects":[],"cross_repo":{"dependency_edges":[]}})"},
      {"workspace_name",
       R"({"schema_version":1,"workspace_id":7,"workspace_slug":"zz","generated_at":"GEN","projects":[],"cross_repo":{"dependency_edges":[]}})"},
      {"generated_at",
       R"({"schema_version":1,"workspace_id":7,"workspace_slug":"zz","workspace_name":"ZZ","projects":[],"cross_repo":{"dependency_edges":[]}})"},
      {"cross_repo",
       R"({"schema_version":1,"workspace_id":7,"workspace_slug":"zz","workspace_name":"ZZ","generated_at":"GEN","projects":[]})"},
  }};
  for (const auto& item : cases) {
    INFO("required member removed: " << item.key);
    auto decoded = routing::decode(item.without);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == routing::decode_error::invalid);
  }
}

TEST_CASE("a required field of the WRONG TYPE reads as absent", "[engine][workspace][routing][decode]") {
  // zig's getInteger tests `!= .integer`, so a float does NOT satisfy an
  // integer field even though it is numerically fine.
  CHECK_FALSE(routing::decode(R"({"schema_version":1.0,"workspace_id":7,"workspace_slug":"zz",)"
                              R"("workspace_name":"ZZ","generated_at":"GEN","projects":[],)"
                              R"("cross_repo":{"dependency_edges":[]}})")
                  .has_value());
  // And a string where an integer belongs.
  CHECK_FALSE(routing::decode(R"({"schema_version":1,"workspace_id":"7","workspace_slug":"zz",)"
                              R"("workspace_name":"ZZ","generated_at":"GEN","projects":[],)"
                              R"("cross_repo":{"dependency_edges":[]}})")
                  .has_value());
}

TEST_CASE("a project without planar_focus fails the WHOLE decode", "[engine][workspace][routing][decode]") {
  // Oracle-captured at exit 2. Note this is the only per-project member that
  // can fail a decode; the rest default.
  auto decoded = routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                                 R"("workspace_name":"A","generated_at":"T","projects":[{"slug":"p"}],)"
                                 R"("cross_repo":{"dependency_edges":[]}})");
  REQUIRE_FALSE(decoded.has_value());
  CHECK(decoded.error() == routing::decode_error::invalid);

  // The same project WITH a focus decodes, which is what makes the assertion
  // above about `planar_focus` rather than about the fixture being malformed.
  auto ok = routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                            R"("workspace_name":"A","generated_at":"T",)"
                            R"("projects":[{"slug":"p","planar_focus":{}}],)"
                            R"("cross_repo":{"dependency_edges":[]}})");
  REQUIRE(ok.has_value());
  REQUIRE(ok->projects.size() == 1);
  CHECK(ok->projects[0].slug == "p");
  // An EMPTY focus object is legal; its counts default to zero.
  CHECK(ok->projects[0].focus.open_tasks == 0);
  CHECK(ok->projects[0].focus.open_questions == 0);
}

TEST_CASE("cross_repo without dependency_edges fails, but its id arrays default", "[engine][workspace][routing][decode]") {
  CHECK_FALSE(routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                              R"("workspace_name":"A","generated_at":"T","projects":[],)"
                              R"("cross_repo":{}})")
                  .has_value());

  // With the required array present, the two id arrays may be absent.
  auto ok = routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                            R"("workspace_name":"A","generated_at":"T","projects":[],)"
                            R"("cross_repo":{"dependency_edges":[]}})");
  REQUIRE(ok.has_value());
  CHECK(ok->cross.plans_scoped_to_org.empty());
  CHECK(ok->cross.questions_scoped_to_org.empty());
}

TEST_CASE("generator_version defaults to the static tag when absent", "[engine][workspace][routing][decode]") {
  auto decoded = routing::decode(k_minimal);
  REQUIRE(decoded.has_value());
  // Typed out rather than compared against the module's own constant.
  CHECK(decoded->generator_version == "static-v1");

  // A supplied value wins — proving the default is a fallback, not a
  // hardcode that would pass either way.
  auto explicit_version = routing::decode(R"({"schema_version":1,"workspace_id":7,"workspace_slug":"zz",)"
                                          R"("workspace_name":"ZZ","generated_at":"GEN",)"
                                          R"("generator_version":"gv-9","projects":[],)"
                                          R"("cross_repo":{"dependency_edges":[]}})");
  REQUIRE(explicit_version.has_value());
  CHECK(explicit_version->generator_version == "gv-9");
}

TEST_CASE("array elements of the wrong type are skipped INDIVIDUALLY", "[engine][workspace][routing][decode]") {
  auto decoded = routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                                 R"("workspace_name":"A","generated_at":"T","projects":[{)"
                                 R"("slug":"p","capabilities":["keep",7,null,"also"],)"
                                 R"("languages":{"go":0.5,"bad":"x","int":2},)"
                                 R"("planar_focus":{"active_plans":[1,"no",3]}}],)"
                                 R"("cross_repo":{"dependency_edges":[)"
                                 R"("not-an-object",{"from":"a","to":"b","reason":"r"}]}})");
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->projects.size() == 1);
  const auto& project = decoded->projects[0];

  // The two strings survive; the integer and the null are dropped.
  CHECK(project.capabilities == std::vector<std::string>{"keep", "also"});
  // The non-numeric language is dropped; an INTEGER share is widened.
  REQUIRE(project.languages.size() == 2);
  CHECK(project.languages[0].first == "go");
  CHECK(project.languages[0].second == 0.5);
  CHECK(project.languages[1].first == "int");
  CHECK(project.languages[1].second == 2.0);
  // The non-integer plan id is dropped.
  CHECK(project.focus.active_plans == std::vector<std::int64_t>{1, 3});
  // The non-object edge is dropped, the object one kept.
  REQUIRE(decoded->cross.dependency_edges.size() == 1);
  CHECK(decoded->cross.dependency_edges[0].from == "a");
}

TEST_CASE("languages preserves the file's key ORDER, not a sort", "[engine][workspace][routing][decode]") {
  // The oracle emits hash order on the write side (see routing.cppm), so the
  // decoder's contract is round-tripping whatever order arrives. A fixture in
  // deliberately non-alphabetical order proves no sort sneaks in.
  auto decoded = routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                                 R"("workspace_name":"A","generated_at":"T","projects":[{)"
                                 R"("slug":"p","languages":{"zeta":0.5,"alpha":0.3,"mid":0.2},)"
                                 R"("planar_focus":{}}],"cross_repo":{"dependency_edges":[]}})");
  REQUIRE(decoded.has_value());
  const auto& languages = decoded->projects[0].languages;
  REQUIRE(languages.size() == 3);
  CHECK(languages[0].first == "zeta");
  CHECK(languages[1].first == "alpha");
  CHECK(languages[2].first == "mid");
}

// ---------------------------------------------------------------------------
// the text renderer
// ---------------------------------------------------------------------------

TEST_CASE("the full text render is byte-identical to the oracle's", "[engine][workspace][routing][show]") {
  auto decoded = routing::decode(k_full);
  REQUIRE(decoded.has_value());

  // Typed out from the captured bytes, NOT rebuilt from the fixture.
  constexpr std::string_view expected = "workspace: org:acme (id 3)\n"
                                        "generated: 2020-01-01T00:00:00Z (gv-9)\n"
                                        "projects:  2\n"
                                        "\n"
                                        "- alpha\n"
                                        "    path:         /r/alpha\n"
                                        "    capabilities: go-service, rust\n"
                                        "    summary:      (no summary)\n"
                                        "    depends_on:   beta\n"
                                        "    open tasks:   4\n"
                                        "    open Qs:      5\n"
                                        "- beta\n"
                                        "    path:         /r/beta\n"
                                        "    capabilities: (none)\n"
                                        "    summary:      Beta repo.\n"
                                        "    open tasks:   0\n"
                                        "    open Qs:      0\n"
                                        "\n"
                                        "cross-repo edges:\n"
                                        "  alpha -> beta (go.mod replace)\n";
  CHECK(routing::render_text(*decoded) == expected);
}

TEST_CASE("an empty table still renders its three header lines and a blank one", "[engine][workspace][routing][show]") {
  auto decoded = routing::decode(k_minimal);
  REQUIRE(decoded.has_value());
  // Oracle-captured verbatim, including the trailing blank line that has no
  // project list to separate it from.
  CHECK(routing::render_text(*decoded) == "workspace: org:zz (id 7)\n"
                                          "generated: GEN (static-v1)\n"
                                          "projects:  0\n"
                                          "\n");
}

TEST_CASE("the summary line keys on the SUMMARY, not on summary_source", "[engine][workspace][routing][show]") {
  // This case exists because a break-probe SURVIVED without it: in the
  // k_full fixture `summary` and `summary_source` are empty together and
  // non-empty together, so swapping which one the renderer tests changed
  // nothing. Two projects that disagree in OPPOSITE directions are what
  // discriminates.
  //
  // Oracle-captured against a fixture with exactly these two projects:
  //   - summary "" with summary_source "readme"  -> `(no summary)`
  //   - summary "Has text." with summary_source "" -> `Has text.`
  auto decoded = routing::decode(R"({"schema_version":1,"workspace_id":1,"workspace_slug":"a",)"
                                 R"("workspace_name":"A","generated_at":"T","generator_version":"gv",)"
                                 R"("projects":[)"
                                 R"({"slug":"sourced-but-blank","root_path":"/r/1","summary":"",)"
                                 R"("summary_source":"readme","planar_focus":{}},)"
                                 R"({"slug":"blank-source","root_path":"/r/2","summary":"Has text.",)"
                                 R"("summary_source":"","planar_focus":{}}],)"
                                 R"("cross_repo":{"dependency_edges":[]}})");
  REQUIRE(decoded.has_value());
  const auto rendered = routing::render_text(*decoded);

  // A project WITH a source but no text still reads `(no summary)` ...
  CHECK(rendered.contains("- sourced-but-blank\n"
                          "    path:         /r/1\n"
                          "    capabilities: (none)\n"
                          "    summary:      (no summary)\n"));
  // ... and one with text but NO source prints the text.
  CHECK(rendered.contains("- blank-source\n"
                          "    path:         /r/2\n"
                          "    capabilities: (none)\n"
                          "    summary:      Has text.\n"));
}

TEST_CASE("depends_on is the only per-project line that can be omitted", "[engine][workspace][routing][show]") {
  auto decoded = routing::decode(k_full);
  REQUIRE(decoded.has_value());
  const auto rendered = routing::render_text(*decoded);

  // PRESENT case: alpha has a dep and prints the line.
  CHECK(rendered.contains("    depends_on:   beta\n"));
  // ABSENT case: beta has none. Counting occurrences rather than asserting
  // non-containment keeps this from passing merely because the substring is
  // missing everywhere — exactly ONE of the two projects prints it.
  std::size_t occurrences = 0;
  for (std::size_t at = rendered.find("    depends_on:"); at != std::string::npos;
       at             = rendered.find("    depends_on:", at + 1)) {
    ++occurrences;
  }
  CHECK(occurrences == 1);

  // Meanwhile BOTH projects print capabilities and summary, so the omission
  // above is specific to depends_on rather than a general empty-field rule.
  std::size_t capability_lines = 0;
  for (std::size_t at = rendered.find("    capabilities: "); at != std::string::npos;
       at             = rendered.find("    capabilities: ", at + 1)) {
    ++capability_lines;
  }
  CHECK(capability_lines == 2);
}

TEST_CASE("the cross-repo block is omitted entirely when there are no edges", "[engine][workspace][routing][show]") {
  auto decoded = routing::decode(k_minimal);
  REQUIRE(decoded.has_value());
  CHECK_FALSE(routing::render_text(*decoded).contains("cross-repo edges:"));

  // The PRESENT case, so the absence above is not passing for the wrong
  // reason (e.g. a renderer that never emits the block at all).
  auto with_edges = routing::decode(k_full);
  REQUIRE(with_edges.has_value());
  CHECK(routing::render_text(*with_edges).contains("\ncross-repo edges:\n"));
}

// ---------------------------------------------------------------------------
// the missing-file refusal
// ---------------------------------------------------------------------------

TEST_CASE("the missing-table refusal names the path it looked for", "[engine][workspace][routing][show]") {
  CHECK(routing::missing_table_error("/w/1/routing-table.json") ==
        "routing table not found at /w/1/routing-table.json; run `planar workspace routing build` first");
  // The path is INTERPOLATED, not decorative — a different path changes the
  // message. `workspace regenerate`'s equivalent omits the path entirely, so
  // these two must never be factored together.
  CHECK(routing::missing_table_error("/other.json") ==
        "routing table not found at /other.json; run `planar workspace routing build` first");
}
