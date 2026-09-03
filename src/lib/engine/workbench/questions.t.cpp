// @file questions.t.cpp
// @brief Unit tests for `planar.engine.workbench.questions` (plan 996,
// task 6302).
//
// ## Every expectation here came from RUNNING the oracle
//
// Captured against `zig/zig-out/bin/planar` in a pinned scratch arena
// (`PLANAR_DB`, `PLANAR_HOME`, `PLANAR_CONFIG_PATH`, `PLANAR_LOCAL_HOME`,
// `PLANAR_WORKBENCH_ROOT` and `HOME` all redirected), by seeding artifacts
// whose bodies carry the sections below, running `workbench push` and then
// `workbench extract-questions --json`. Every invocation went through a
// PIPE on both streams with the exit code read OUTSIDE it, per
// `../../../cmd/parity_harness.hpp`.
//
// The `source_line` numbers below (12, 13, 14, 17) look arbitrary and are
// not: they are 1-based within the PARSED BODY, which begins after the
// front-matter delimiter, and the renderer puts six lines of heading and
// `**Kind:**` scaffolding before `## Content`. They were read off the
// oracle, never computed from the fixture.
//
// ## THE VACUOUS-FIXTURE TRAP THIS FILE EXISTS TO AVOID
//
// `extract_questions` returns an empty vector for any body with no
// `## Open Questions` H2, and `collect_top_level_specs` returns an empty
// list for any directory that is a README plus subdirectories — which is
// what a workbench feature tree looks like before an artifact is added to
// it. Both are the SHAPE OF A PASSING TEST THAT ASSERTS NOTHING, and both
// have burned cycles on this milestone.
//
// So the cases below never assert only a property of the result; each one
// asserts the extraction is non-empty FIRST and pins the contents second.
// The two deliberate empty-result cases are named as such.
//
// ## BREAK-PROBES
//
// Run via `scripts/break-probe.sh`. Each mutant built and was KILLED by the
// single named test; no survivors:
//
//   always take the bullet branch (drop the H3 dispatch)
//       -> "the H3 branch takes the heading as title and the lines
//           beneath as body"                                       killed
//   never terminate the section at the next H1/H2
//       -> "the section stops at the next H1 or H2, so later
//           bullets are not questions"                             killed
//   stop skipping README.md in the collector
//       -> "extract-questions skips README.md and does not descend"  killed

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.questions;

namespace {

namespace q = planar::engine::workbench::questions;

/// @brief A body with a bullet-shaped Open Questions section.
///
/// Reproduces the oracle fixture: a long first bullet that DOES split at
/// `? `, a short one that does not split at all, a NESTED bullet (which
/// counts — `is_bullet` trims leading whitespace), and a following `## `
/// section whose bullet must NOT be picked up.
constexpr std::string_view k_bullet_body = R"(# Artifact 1: Bullet Spec

**Kind:** tech_spec
**Status:** draft

## Content

Intro line.

## Open Questions

- Should we cache the result? It would help a lot on repeated reads and we think it matters.
- What about eviction?
  - nested bullet counts too

## Next Section

- not a question
)";

/// @brief A body with an H3-shaped Open Questions section.
constexpr std::string_view k_h3_body = R"(# Artifact 2: H3 Spec

**Kind:** tech_spec
**Status:** draft

## Content

Preamble.

## Open Questions

### Which serializer?

JSON is the default.
It has two lines.

### Do we version the payload?

Yes, probably.

# Terminator
)";

/// @brief Make a scratch directory unique to one case.
auto make_dir(std::string_view tag) -> std::filesystem::path {
  auto const      dir = std::filesystem::temp_directory_path() /
                        std::format("planar_wbq_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  REQUIRE(!ec);
  return dir;
}

/// @brief Write a file, creating parents.
void write_file(const std::filesystem::path& path, std::string_view body) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  REQUIRE(file.good());
  file << body;
}

} // namespace

TEST_CASE("the bullet branch splits each bullet at its first sentence", "[engine][workbench][questions]") {
  auto const got = q::extract_questions(k_bullet_body);

  // NON-EMPTY FIRST. Every assertion below is vacuous without this, and an
  // `## Open Questions` heading that stopped being recognised would
  // otherwise leave this whole case green.
  REQUIRE(got.size() == 3);

  // The `? ` terminator splits, and the title KEEPS its question mark.
  CHECK(got[0].title == "Should we cache the result?");
  CHECK(got[0].body == "It would help a lot on repeated reads and we think it matters.");
  CHECK(got[0].source_line == 12);

  // A bullet whose only `?` is at end-of-line has no `? ` and does NOT
  // split: it is all title, empty body.
  CHECK(got[1].title == "What about eviction?");
  CHECK(got[1].body.empty());
  CHECK(got[1].source_line == 13);

  // A NESTED bullet is still a question. This is the arm an implementer
  // would most plausibly "fix" into a top-level-only check.
  CHECK(got[2].title == "nested bullet counts too");
  CHECK(got[2].body.empty());
  CHECK(got[2].source_line == 14);
}

TEST_CASE("the earliest terminator wins even when a later k_terms entry matches first in the text",
          "[engine][workbench][questions]") {
  // k_terms is tried in the fixed order {". ", "? ", "! "}. A bullet whose
  // "! " occurs BEFORE its ". " forces the loop to re-run its
  // `punct < *earliest` comparison and actually move `earliest` backward
  // on the third iteration -- a mutant that stops updating after the
  // first match would keep the LATER ". " split point instead.
  constexpr std::string_view body = R"(## Open Questions

- Wait! Are we sure. Let's check.
)";
  auto const got = q::extract_questions(body);
  REQUIRE(got.size() == 1);
  CHECK(got[0].title == "Wait!");
  CHECK(got[0].body == "Are we sure. Let's check.");
}

TEST_CASE("the section stops at the next H1 or H2, so later bullets are not questions", "[engine][workbench][questions]") {
  auto const got = q::extract_questions(k_bullet_body);
  REQUIRE_FALSE(got.empty());
  // `## Next Section` terminates the walk. Asserting the ABSENCE alone
  // would pass if the walk found nothing at all, so the present case is
  // pinned above and the count is pinned here.
  CHECK(got.size() == 3);
  CHECK(std::ranges::none_of(got, [](const q::question& item) { return item.title == "not a question"; }));
}

TEST_CASE("the H3 branch takes the heading as title and the lines beneath as body", "[engine][workbench][questions]") {
  auto const got = q::extract_questions(k_h3_body);
  REQUIRE(got.size() == 2);

  // A MULTI-LINE body is joined with `\n`, with blank lines trimmed off
  // both ends but kept in the middle.
  CHECK(got[0].title == "Which serializer?");
  CHECK(got[0].body == "JSON is the default.\nIt has two lines.");
  CHECK(got[0].source_line == 12);

  CHECK(got[1].title == "Do we version the payload?");
  CHECK(got[1].body == "Yes, probably.");
  CHECK(got[1].source_line == 17);
}

TEST_CASE("a section with any H3 uses the H3 branch for the WHOLE section", "[engine][workbench][questions]") {
  // The branch choice is per-SECTION, not per-line: a bullet sitting under
  // an H3 is body text, not its own question. A port that ran both walks
  // and concatenated would return three here.
  constexpr std::string_view mixed = R"(## Open Questions

### A heading question?

- this bullet is BODY, not a question
)";
  auto const                 got   = q::extract_questions(mixed);
  REQUIRE(got.size() == 1);
  CHECK(got[0].title == "A heading question?");
  CHECK(got[0].body == "- this bullet is BODY, not a question");
}

TEST_CASE("the Open Questions heading is matched case-insensitively", "[engine][workbench][questions]") {
  constexpr std::string_view shouty = "## OPEN QUESTIONS\n\n- Does case matter?\n";
  auto const                 got    = q::extract_questions(shouty);
  REQUIRE(got.size() == 1);
  CHECK(got[0].title == "Does case matter?");
}

TEST_CASE("a body with no Open Questions section yields nothing", "[engine][workbench][questions]") {
  // A DELIBERATE empty-result case, named as one. The non-empty cases
  // above are what keep it from being the only thing this file proves.
  CHECK(q::extract_questions("# Spec\n\nNo questions here.\n").empty());
  CHECK(q::extract_questions("").empty());
  // `## Open Questions Extra` is a DIFFERENT heading; the match is on the
  // whole trimmed heading text, not a prefix.
  CHECK(q::extract_questions("## Open Questions Extra\n\n- nope\n").empty());
}

TEST_CASE("collect_top_level_specs skips README.md, subdirectories and non-Markdown", "[engine][workbench][questions]") {
  auto const dir = make_dir("collect");
  write_file(dir / "README.md", "x");
  write_file(dir / "2-beta.md", "x");
  write_file(dir / "1-alpha.md", "x");
  write_file(dir / "notes.txt", "x");
  write_file(dir / "questions" / "1-nested.md", "x");

  auto const got = q::collect_top_level_specs(dir);

  // NON-EMPTY FIRST, and this is the assertion that matters most in this
  // file: the tree above is exactly the shape a real feature directory has
  // — a README plus a `questions/` subdirectory — and a walk that returned
  // nothing would make every extract-questions test downstream pass while
  // scanning no files at all.
  REQUIRE(got.size() == 2);
  // Sorted bytewise, mirroring the oracle's insertion sort.
  CHECK(got[0] == "1-alpha.md");
  CHECK(got[1] == "2-beta.md");
}

TEST_CASE("collect_top_level_specs treats an absent directory as empty, not an error", "[engine][workbench][questions]") {
  CHECK(q::collect_top_level_specs(make_dir("absent") / "no-such-child").empty());
}

TEST_CASE("render_text elides a body over 60 bytes and omits the dash when there is none", "[engine][workbench][questions]") {
  std::vector<q::file_questions> const results{
      {.artifact_id = 1, .file = "1-bullet-spec.md", .questions = q::extract_questions(k_bullet_body)},
      {.artifact_id = 3, .file = "3-empty-spec.md", .questions = {}},
  };
  REQUIRE_FALSE(results[0].questions.empty());

  auto const got = q::render_text(results);

  // Captured from the oracle verbatim. The first body is 61 bytes, so it
  // is cut at 60 and followed by U+2026 — note the truncated word
  // `matter` where the source says `matters`.
  CHECK(got == "1-bullet-spec.md (artifact 1): 3 question(s)\n"
               "  [line 12] Should we cache the result? — It would help a lot on repeated reads and we think it matter…\n"
               "  [line 13] What about eviction?\n"
               "  [line 14] nested bullet counts too\n"
               "3-empty-spec.md (artifact 3): 0 question(s)\n");
}

TEST_CASE("render_json matches the oracle's Stringify field order and escaping", "[engine][workbench][questions]") {
  std::vector<q::file_questions> const results{
      {.artifact_id = 2, .file = "2-h3-spec.md", .questions = q::extract_questions(k_h3_body)},
  };
  REQUIRE(results[0].questions.size() == 2);

  // The embedded newline in the first body is what makes this more than a
  // field-order check: it must arrive as the two-character `\n` escape,
  // which is what `std.json.Stringify` emits and what `json_text` was
  // built to match.
  CHECK(q::render_json(results) ==
        R"([{"artifact_id":2,"file":"2-h3-spec.md","questions":[)"
        R"({"title":"Which serializer?","body":"JSON is the default.\nIt has two lines.","source_line":12},)"
        R"({"title":"Do we version the payload?","body":"Yes, probably.","source_line":17}]}])");
}

TEST_CASE("render_json spells an empty result set as [] and an empty question list as []", "[engine][workbench][questions]") {
  CHECK(q::render_json(std::vector<q::file_questions>{}) == "[]");
  std::vector<q::file_questions> const one{{.artifact_id = 3, .file = "3-empty-spec.md", .questions = {}}};
  CHECK(q::render_json(one) == R"([{"artifact_id":3,"file":"3-empty-spec.md","questions":[]}])");
}
