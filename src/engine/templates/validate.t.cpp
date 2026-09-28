// @file validate.t.cpp
// @brief Template-validation tests for
// `planar.engine.templates.validate` (plan 996, task 6190).
//
// HOME SAFETY. Pure in-memory work over a decoded tree. No file, no
// environment variable, no path.
//
// ORACLE PROVENANCE. The issue ORDER, the dotted-path spelling, and the
// duplicate `(smoke-render)` entry were all read off
// `templates validate probe px broken` run against the built Zig binary in
// a pinned scratch arena.
//
// THE DOUBLE REPORT IS THE CONTRACT. `validate` walks every string field
// AND then smoke-renders the whole template, so a broken field is reported
// TWICE — once against its own JSON path and once against the pseudo-path
// `(smoke-render)`. The oracle's own summary line counts both, so
// collapsing the duplicate would change the number on stderr. See
// validate.cppm.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.json_dom;
import planar.engine.templates.validate;

namespace tpl = planar::engine::templates;
namespace jd  = planar::json_dom;

namespace {

/// @brief Parse `text` and validate it.
/// @param text A template document.
/// @return The issues, in document order.
auto issues_for(std::string_view text) -> std::vector<tpl::validation_issue> {
  auto parsed = jd::parse_json(text);
  REQUIRE(parsed.has_value());
  return tpl::validate_template(*parsed);
}

} // namespace

TEST_CASE("validate reports NO issues for a clean template", "[templates][validate]") {
  CHECK(issues_for(R"({"title":"{{.Task.Title}}","labels":["a","b"],"n":1})").empty());
}

TEST_CASE("validate reports a broken field TWICE — once by path, once as (smoke-render)", "[templates][validate]") {
  // Oracle: five broken fields yielded SIX issues, and the summary said
  // `6 issue(s)`.
  auto const issues = issues_for(R"({"a":"{{.Nope}}"})");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].json_path == "a");
  CHECK(issues[0].message == "UnknownField");
  CHECK(issues[1].json_path == "(smoke-render)");
  CHECK(issues[1].message == "smoke render failed: UnknownField");
}

TEST_CASE("validate reports issues in DOCUMENT order with (smoke-render) last", "[templates][validate]") {
  // Oracle bytes from `probe px broken`, whose five fields came back
  // a, b, c, d, e, then the smoke pass.
  auto const issues = issues_for(
      R"({"a":"{{.Nope}}","b":"{{.Touches}}","c":"{{if .ExternalKey}}x","d":"{{end}}","e":"{{with .X}}y{{end}}","f":"ok"})");
  REQUIRE(issues.size() == 6);
  CHECK(issues[0].json_path == "a");
  CHECK(issues[0].message == "UnknownField");
  CHECK(issues[1].json_path == "b");
  CHECK(issues[1].message == "UnsupportedDirective");
  CHECK(issues[2].json_path == "c");
  CHECK(issues[2].message == "UnclosedDirective");
  CHECK(issues[3].json_path == "d");
  CHECK(issues[3].message == "UnsupportedDirective");
  CHECK(issues[4].json_path == "e");
  CHECK(issues[4].message == "UnsupportedDirective");
  CHECK(issues[5].json_path == "(smoke-render)");
  // `f` is clean and gets no entry at all.
}

TEST_CASE("validate's dotted path is the BARE key at the top level", "[templates][validate][path]") {
  // No leading dot. Only NESTED members get the `parent.child` join.
  auto const issues = issues_for(R"({"top":"{{.Nope}}"})");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].json_path == "top");
}

TEST_CASE("validate joins NESTED object paths with a dot", "[templates][validate][path]") {
  auto const issues = issues_for(R"({"fields":{"summary":"{{.Nope}}"}})");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].json_path == "fields.summary");
}

TEST_CASE("validate indexes ARRAY elements with brackets", "[templates][validate][path]") {
  auto const issues = issues_for(R"({"labels":["ok","{{.Nope}}"]})");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].json_path == "labels[1]");
}

TEST_CASE("validate indexes a ROOT-level array with no prefix", "[templates][validate][path]") {
  auto const issues = issues_for(R"([{"k":"{{.Nope}}"}])");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].json_path == "[0].k");
}

TEST_CASE("validate ignores non-string leaves", "[templates][validate]") {
  // Numbers, booleans and nulls carry no directives, so a template made
  // only of them is clean rather than unreachable.
  CHECK(issues_for(R"({"n":42,"b":true,"z":null,"empty":{},"arr":[]})").empty());
}

TEST_CASE("validate exercises the STUB context, so a field-reference template is clean", "[templates][validate][stub]") {
  // The stub must populate every reachable field. A stub that left, say,
  // `Assoc` empty would still validate this clean — but `{{if .Assoc.Slug}}`
  // below would stop entering its body, which is the case that matters.
  CHECK(issues_for(R"({"a":"{{.Feature.Title}}{{.Plan.Slug}}{{.Task.Body}}{{.Scenario.ID}}{{.Assoc.Name}}{{.ExternalKey}}"})")
            .empty());
}

TEST_CASE("validate ENTERS an if body against the stub, so a broken directive inside is found", "[templates][validate][stub]") {
  // The load-bearing consequence of a fully-populated stub. If
  // `stub_context().assoc.slug` were empty this template would validate
  // clean and the broken `{{.Nope}}` would ship.
  auto const issues = issues_for(R"({"a":"{{if .Assoc.Slug}}{{.Nope}}{{end}}"})");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].message == "UnknownField");
}

TEST_CASE("validate ENTERS a Touches range against the stub", "[templates][validate][stub]") {
  // `stub_context().touches` holds one entry for exactly this reason.
  auto const issues = issues_for(R"({"a":"{{range .Touches}}{{.Nope}}{{end}}"})");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].message == "UnknownField");
}

TEST_CASE("validate does NOT enter a Children range — a reproduced blind spot", "[templates][validate][stub][oracle-defect]") {
  // `stub_context().children` is EMPTY, matching the oracle, so a
  // `{{range .Children}}` body is never rendered during validation and a
  // broken directive inside one is INVISIBLE to `templates validate`.
  //
  // Reproduced rather than corrected: populating the stub here would make
  // this tree reject templates the oracle accepts, which is a divergence
  // on `templates validate`'s exit code. Named so the blind spot is not
  // mistaken for coverage.
  CHECK(issues_for(R"({"a":"{{range .Children}}{{.Nope}}{{end}}"})").empty());
}
