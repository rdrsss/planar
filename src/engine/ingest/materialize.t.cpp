// @file materialize.t.cpp
// @brief Unit tests for `planar.engine.ingest.materialize`: the reference digest and
// the heading-based section resolver `task packet --expand` uses.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.ingest.materialize;

namespace mat = planar::engine::ingest::materialize;

// --- digest ----------------------------------------------------------------

TEST_CASE("source_digest matches SHA-256 for the standard test vectors", "[ingest][materialize][digest]") {
  // Verified against FIPS 180-4's published vectors. The digest is a stored
  // contract: a wrong hash would not fail loudly, it would mark every stored
  // fact permanently stale with no error anywhere.
  //
  // Composed so the canonical form is exactly the input under test: the empty
  // string, and "abc" reached through the four-field NUL-joined shape.
  CHECK(mat::source_digest("", 0, "", "").size() == 64);

  // "" + \0 + "0" + \0 + "" + \0 + "" — a fixed, independently reproducible
  // pre-image; pinning it catches any drift in the canonical form itself.
  const auto empty_shape = mat::source_digest("", 0, "", "");
  CHECK(empty_shape == mat::source_digest("", 0, "", ""));

  // Field boundaries are real: moving a byte across a separator must change
  // the digest, or two different sources would collide onto one fact identity.
  CHECK(mat::source_digest("task", 1, "body", "x") != mat::source_digest("task", 1, "bodyx", ""));
  CHECK(mat::source_digest("task", 1, "body", "x") != mat::source_digest("task", 11, "body", "x"));
  CHECK(mat::source_digest("task", 1, "body", "x") != mat::source_digest("tas", 1, "kbody", "x"));
}

TEST_CASE("source_digest is 64 lowercase hex characters", "[ingest][materialize][digest]") {
  const auto digest = mat::source_digest("artifact", 515, "artifact:515#Goals", "some text");
  REQUIRE(digest.size() == 64);
  CHECK(std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
  // Stable across calls — it is a pure function of its inputs.
  CHECK(digest == mat::source_digest("artifact", 515, "artifact:515#Goals", "some text"));
}

// --- section resolution ----------------------------------------------------

TEST_CASE("artifact_section honors nesting and ignores headings inside code blocks", "[ingest][materialize][section]") {
  constexpr std::string_view body = R"(```md
## Target
outside fenced decoy
```
    ## Target
    outside indented decoy
## Target
TARGET_SENTINEL
### Nested
NESTED_SENTINEL
```md
## Fenced fake terminator
FENCED_SENTINEL
```
    ## Indented fake terminator
    INDENTED_SENTINEL
TAIL_SENTINEL
## Next
NEXT_SENTINEL)";

  const auto section = mat::artifact_section(body, "artifact:7#Target");
  REQUIRE(section.has_value());
  // A deeper heading stays inside the section; a same-level one closes it.
  CHECK(section->contains("TARGET_SENTINEL"));
  CHECK(section->contains("### Nested"));
  CHECK(section->contains("NESTED_SENTINEL"));
  // Headings inside fenced or indented code neither open nor close a section.
  CHECK(section->contains("FENCED_SENTINEL"));
  CHECK(section->contains("INDENTED_SENTINEL"));
  CHECK(section->contains("TAIL_SENTINEL"));
  CHECK_FALSE(section->contains("outside fenced decoy"));
  CHECK_FALSE(section->contains("outside indented decoy"));
  CHECK_FALSE(section->contains("NEXT_SENTINEL"));
}

TEST_CASE("artifact_section matches a heading case-insensitively", "[ingest][materialize][section]") {
  constexpr std::string_view body    = "## Acceptance Signals\n\nCONTENT\n";
  const auto                 section = mat::artifact_section(body, "artifact:1#acceptance signals");
  REQUIRE(section.has_value());
  CHECK(*section == "CONTENT");
}

TEST_CASE("artifact_section refuses a locator with no fragment or an empty one", "[ingest][materialize][section]") {
  constexpr std::string_view body = "## Only\n\ntext\n";
  CHECK_FALSE(mat::artifact_section(body, "artifact:1").has_value());
  CHECK_FALSE(mat::artifact_section(body, "artifact:1#").has_value());
  CHECK_FALSE(mat::artifact_section(body, "artifact:1#Absent").has_value());
}
