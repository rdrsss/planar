// @file toml.t.cpp
// @brief Unit tests for `planar.engine.config.toml` (plan 996, task 6032).
// Exercises the Glaze-backed TOML-to-dotted-map flattener against the same
// shapes zig/src/engine/config/parse.zig's own test suite pins (section
// headers, dotted/quoted table headers, string arrays, ints, bools) plus
// the unsupported-shape rejections (float values, non-string array
// elements) this module's restricted schema does not accept.
#include <catch2/catch_test_macros.hpp>
#include <glaze/json/generic.hpp>
#include <glaze/toml.hpp>

import std;
import planar.engine.config.toml;

using planar::engine::config::parse_toml;
using planar::engine::config::toml_error;
using planar::engine::config::toml_value;

TEST_CASE("parse_toml: empty document returns an empty map", "[toml]") {
  auto result = parse_toml("");
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("parse_toml: comments-only document returns an empty map", "[toml]") {
  auto result = parse_toml("# a comment\n# another\n");
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("parse_toml: top-level string key", "[toml]") {
  auto result = parse_toml("vendor = \"claude\"\n");
  REQUIRE(result.has_value());
  auto it = result->find("vendor");
  REQUIRE(it != result->end());
  CHECK(it->second.kind_ == toml_value::kind::string);
  CHECK(it->second.string_ == "claude");
}

TEST_CASE("parse_toml: section header creates dotted keys", "[toml]") {
  auto result = parse_toml("[defaults]\nvendor = \"claude\"\nscope = \"global\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("defaults.vendor").string_ == "claude");
  CHECK(result->at("defaults.scope").string_ == "global");
}

TEST_CASE("parse_toml: dotted table header [external.jira.status]", "[toml]") {
  auto result = parse_toml("[external.jira.status]\ntodo = \"To Do\"\ndoing = \"In Progress\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("external.jira.status.todo").string_ == "To Do");
  CHECK(result->at("external.jira.status.doing").string_ == "In Progress");
}

TEST_CASE("parse_toml: quoted table header key flattens to the same bare dotted key", "[toml]") {
  auto result = parse_toml("[external.\"github-issues\"]\nauth = \"gh-cli\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("external.github-issues.auth").string_ == "gh-cli");
}

TEST_CASE("parse_toml: bool true and false", "[toml]") {
  auto result = parse_toml("enabled = true\ndisabled = false\n");
  REQUIRE(result.has_value());
  CHECK(result->at("enabled").kind_ == toml_value::kind::boolean);
  CHECK(result->at("enabled").bool_ == true);
  CHECK(result->at("disabled").bool_ == false);
}

TEST_CASE("parse_toml: integer value", "[toml]") {
  auto result = parse_toml("count = 42\n");
  REQUIRE(result.has_value());
  CHECK(result->at("count").kind_ == toml_value::kind::integer);
  CHECK(result->at("count").int_ == 42);
}

TEST_CASE("parse_toml: string array", "[toml]") {
  auto result = parse_toml("parent_field_names = [\"Parent\", \"Initiative\", \"Tracking\"]\n");
  REQUIRE(result.has_value());
  const auto& v = result->at("parent_field_names");
  REQUIRE(v.kind_ == toml_value::kind::array);
  REQUIRE(v.array_.size() == 3);
  CHECK(v.array_[0] == "Parent");
  CHECK(v.array_[1] == "Initiative");
  CHECK(v.array_[2] == "Tracking");
}

TEST_CASE("parse_toml: a non-string array element is rejected", "[toml]") {
  auto result = parse_toml("mixed = [\"a\", 1]\n");
  REQUIRE_FALSE(result.has_value());
  // The offending element is named (task 6266): the oracle's hand-rolled
  // tokenizer fails on the first raw character it sees, which for an
  // integer element is that integer's leading digit.
  CHECK(result.error().message == "only string arrays are supported; got '1'");
}

TEST_CASE("parse_toml: the offending element is named across every non-string TOML type", "[toml]") {
  // One case per `offending_char` branch, so the fix cannot be reproducing
  // only the single-digit-integer case its own reproduction step used.
  struct case_t {
    std::string_view doc;
    char             expect;
  };
  for (auto const& c : std::array<case_t, 5>{{
           {"mixed = [\"a\", 42]\n", '4'},   // multi-digit: NOT the whole "42"
           {"mixed = [\"a\", -3]\n", '-'},   // negative: the sign, not the digit
           {"mixed = [\"a\", true]\n", 't'}, // boolean
           {"mixed = [\"a\", 1.5]\n", '1'},  // float
           {"mixed = [\"a\", [1]]\n", '['},  // nested array
       }}) {
    INFO("doc: " << c.doc);
    auto result = parse_toml(c.doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == std::format("only string arrays are supported; got '{}'", c.expect));
  }
}

TEST_CASE("parse_toml: a float value is rejected", "[toml]") {
  auto result = parse_toml("ratio = 0.5\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().message == "float values are not supported");
}

TEST_CASE("parse_toml: malformed document is rejected", "[toml]") {
  auto result = parse_toml("vendor = \"unclosed\n");
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().message.empty());
}

// ---------------------------------------------------------------------------
// Located parse diagnostics (task 6081). zig/src/engine/config/parse.zig
// carries line+column+message on every failure and
// zig/src/cmd/planar/handlers/config/validate.zig:38-55 prints them as
// "error: line {line}: col {column}: TOML parse error: {message}". The port
// collapsed all of that to a single enumerator; these pin its restoration.
// Oracle transcript (zig/zig-out/bin/planar config validate, pinned arena):
//   "[defaults]\nx = 1.5\n"      -> line 2: col 6: float values are not supported
//   "vendor = \"unclosed\n"      -> line 2: col 1: unterminated string (newline in string)
// The columns differ by design: the two front ends do not share a grammar,
// so toml_error promises an EQUIVALENT location, not an identical one.
// ---------------------------------------------------------------------------

TEST_CASE("parse_toml: a float rejection carries the offending line and column", "[toml]") {
  auto result = parse_toml("[defaults]\nx = 1.5\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().line == 2);
  CHECK(result.error().column == 5); // The '1' of "1.5"; zig points one further in, at the '.'.
  CHECK(result.error().message == "float values are not supported");
}

TEST_CASE("parse_toml: a non-string array rejection carries the offending line", "[toml]") {
  auto result = parse_toml("[defaults]\nmixed = [\"a\", 1]\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().line == 2);
  CHECK(result.error().column == 9);
  CHECK(result.error().message == "only string arrays are supported; got '1'");
}

TEST_CASE("parse_toml: a syntax error carries a line, a column and a message", "[toml]") {
  auto result = parse_toml("[defaults]\nvendor = \"unclosed\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().line > 0);
  CHECK(result.error().column > 0);
  CHECK_FALSE(result.error().message.empty());
}

TEST_CASE("parse_toml: the reported line is the SOURCE line, not the normalized one", "[toml]") {
  // normalize_sections reorders sections shallowest-first, so the float on
  // source line 5 moves to a different line in the text Glaze reads. The
  // diagnostic must still name line 5.
  auto result = parse_toml("[a.b]\nk = \"v\"\n[a]\nj = \"w\"\n[c]\nx = 1.5\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().line == 6);
}

// ---------------------------------------------------------------------------
// Table ordering and duplicates (task 6082). Each case below was measured
// against the Zig oracle in a pinned scratch arena before being pinned here.
// ---------------------------------------------------------------------------

TEST_CASE("parse_toml: a super-table may follow its own sub-table", "[toml]") {
  // Legal TOML, and `zig ... config validate` exits 0 on it. Glaze's
  // ensure_map_path rejects it outright ("Re-defining an already-defined
  // table is invalid", toml/read.hpp:2010-2012), which is why
  // normalize_sections exists. If a Glaze bump ever makes the raw ordering
  // work, this test keeps passing and normalize_sections becomes redundant
  // rather than wrong.
  auto result = parse_toml("[external.jira.status]\ntodo = \"To Do\"\n[external.jira]\nbase_url = \"https://x\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("external.jira.status.todo").string_ == "To Do");
  CHECK(result->at("external.jira.base_url").string_ == "https://x");
}

TEST_CASE("parse_toml: the sub-table-after-super-table ordering still works", "[toml]") {
  auto result = parse_toml("[external.jira]\nbase_url = \"https://x\"\n[external.jira.status]\ntodo = \"To Do\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("external.jira.status.todo").string_ == "To Do");
  CHECK(result->at("external.jira.base_url").string_ == "https://x");
}

TEST_CASE("parse_toml: a repeated table header with disjoint keys merges", "[toml]") {
  // `zig ... config validate` exits 0: parse.zig has no redefinition
  // concept, a header just re-points `current_section`.
  auto result = parse_toml("[defaults]\nvendor = \"claude\"\n[defaults]\nscope = \"g\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("defaults.vendor").string_ == "claude");
  CHECK(result->at("defaults.scope").string_ == "g");
}

TEST_CASE("parse_toml: a duplicate key is rejected (deliberate divergence from zig)", "[toml]") {
  // zig's parse.zig:359-367 documents last-wins overwrite and the oracle
  // resolves `defaults.vendor = codex` here. The TOML SPECIFICATION makes a
  // duplicate key an error, so Glaze is conformant and zig is not; this port
  // stays strict on purpose. Changing this back to last-wins would be a
  // silent spec violation, so the divergence is pinned rather than papered
  // over. See toml.cppm's "Known, deliberate divergences" section.
  auto result = parse_toml("[defaults]\nvendor = \"claude\"\nvendor = \"codex\"\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().line > 0);
}

TEST_CASE("normalize_sections: a bracketed line inside a multi-line string is not a header", "[toml]") {
  auto result = parse_toml("x = \"\"\"\n[not a header]\n\"\"\"\ny = 1\n");
  REQUIRE(result.has_value());
  CHECK(result->at("y").int_ == 1);
  CHECK(result->contains("not a header") == false);
}

TEST_CASE("normalize_sections: emits shallower headers before the deeper ones they extend", "[toml]") {
  auto doc = planar::engine::config::normalize_sections("[a.b]\nk = \"v\"\n[a]\nj = \"w\"\n");
  CHECK(doc.text.find("[a]") < doc.text.find("[a.b]"));
  // Every emitted line maps back to a real source line. The input's trailing
  // newline yields a final empty line (source line 5), which is preserved.
  REQUIRE(doc.line_origin.size() == 5);
  CHECK(std::ranges::max(doc.line_origin) == 5);
  CHECK(std::ranges::is_permutation(doc.line_origin, std::vector<std::uint32_t>{1, 2, 3, 4, 5}));
}

// ---------------------------------------------------------------------------
// Glaze generic-root dispatch (task 6076). parse_toml reads the document
// root into a std::map<std::string, glz::generic_i64> rather than a bare
// glz::generic_i64 because the latter unwraps to its internal variant and
// dispatches through Glaze's VARIANT reader, whose `case '['` branch treats
// a leading table header as an array literal (toml/read.hpp:2692). This test
// pins BOTH halves of that claim, so a Glaze bump that fixes the generic
// path — or that breaks the map path — is noticed here rather than in a
// config-load failure. See toml.cppm's header and toml.cpp's call site.
// ---------------------------------------------------------------------------

TEST_CASE("glaze: a bare generic_i64 at the document root still mis-dispatches", "[toml][glaze-pin]") {
  constexpr std::string_view doc = "[defaults]\nvendor = \"claude\"\n";

  glz::generic_i64 bare;
  const auto       bare_ec = glz::read_toml(bare, doc);
  CHECK(static_cast<bool>(bare_ec)); // Still broken upstream: the workaround is still required.

  std::map<std::string, glz::generic_i64, std::less<>> as_map;
  const auto                                           map_ec = glz::read_toml(as_map, doc);
  CHECK_FALSE(static_cast<bool>(map_ec)); // The readable_map_t path is table-header aware.
  REQUIRE(as_map.contains("defaults"));
}
