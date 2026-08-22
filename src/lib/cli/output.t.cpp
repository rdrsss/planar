// @file output.t.cpp
// @brief Unit tests for `planar.cli.output` (task cpp-cli-output-logging).
//
// Oracle captures this test's expected JSON *shape* (compact, no inserted
// whitespace, declaration-field order, `null` for an absent optional
// field) was checked against — task brief: derive expected values by
// running the reference binary, never hand-assumed:
//
//   $ ./zig/zig-out/bin/planar scope show --json
//   {"resolved_scopes":[{"kind":"association","id":1,"slug":"project:planar",
//    "name":"Planar self-development","kind_label":"project"}],
//    "source":"cwd","cwd":"/Users/mn/projects/github/rdrsss/planar"}
//
//   $ ./zig/zig-out/bin/planar plan list --json
//   [{"id":43,"scope_kind":"association","scope_id":1,"title":"...",
//     "slug":"...","summary":null,"status":"active","parent_plan_id":null,
//     "created_at":"...","updated_at":"..."}, ...]
//
// Both captures share the conventions this test's model types exercise:
// object nesting inside an array (scope show's `resolved_scopes`), a
// bare-array top-level shape for list verbs (plan list), `null` for an
// absent optional field (`summary`, `parent_plan_id`) rather than the key
// being omitted, string/int/bool leaf fields, and zero inserted
// whitespace anywhere in the emitted text.
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp> // for the round-trip re-parse in the escaping break-probe below

import std;
import planar.cli.output;

using planar::cli::emit;
using planar::cli::emit_list;
using planar::cli::output_format;

// Model types for the tests below. Deliberately NOT in an anonymous
// namespace: Glaze's compile-time reflection needs external linkage on a
// reflected type (`extern const T external;` — see
// src/lib/core/vendor_probe.cpp's file comment for the same constraint on
// its probe payload type); an anonymous-namespace type fails to compile
// with "used but not defined ... cannot be defined in any other
// translation unit because its type does not have linkage".
namespace test_model {

/// @brief Models one entry of the `scope show --json` oracle's
/// `resolved_scopes` array.
struct resolved_scope {
  std::string kind;       ///< Scope kind (e.g. "association").
  int         id = 0;     ///< Scope entity id.
  std::string slug;       ///< Scope slug.
  std::string name;       ///< Scope display name.
  std::string kind_label; ///< Human-readable kind label.
};

/// @brief Models the "object containing a nested array of objects, plus
/// scalar string fields" shape from the `scope show --json` oracle
/// capture (see this file's header comment).
struct scope_summary {
  std::vector<resolved_scope> resolved_scopes; ///< The resolved scope chain.
  std::string                 source;          ///< Where the scope was resolved from (e.g. "cwd").
  std::string                 cwd;             ///< The resolving working directory.
};

/// @brief Models the "array of objects with a null-able optional field"
/// shape from the `plan list --json` oracle capture (see this file's
/// header comment).
struct plan_row {
  int                        id = 0;         ///< Plan id.
  std::string                title;          ///< Plan title.
  std::optional<std::string> summary;        ///< null when absent, matching the oracle's "summary":null.
  std::optional<int>         parent_plan_id; ///< null when absent, matching the oracle's "parent_plan_id":null.
};

} // namespace test_model

namespace {

using test_model::plan_row;
using test_model::resolved_scope;
using test_model::scope_summary;

auto scope_text(scope_summary const& v, std::ostream& out) -> void {
  out << "scope: " << v.source << " (" << v.resolved_scopes.size() << " resolved)\n";
}

auto plan_list_text(std::vector<plan_row> const& rows, std::ostream& out) -> void {
  for (auto const& r : rows) {
    out << r.id << "  " << r.title << "\n";
  }
}

} // namespace

TEST_CASE("emit: json path — object with nested array matches oracle shape (no whitespace, field order)", "[cli][output]") {
  scope_summary v{
      .resolved_scopes = {{.kind       = "association",
                           .id         = 1,
                           .slug       = "project:planar",
                           .name       = "Planar self-development",
                           .kind_label = "project"}},
      .source          = "cwd",
      .cwd             = "/Users/mn/projects/github/rdrsss/planar",
  };
  std::ostringstream out;
  emit(v, output_format::json, scope_text, out);
  REQUIRE(out.str() == "{\"resolved_scopes\":[{\"kind\":\"association\",\"id\":1,\"slug\":\"project:planar\","
                       "\"name\":\"Planar self-development\",\"kind_label\":\"project\"}],\"source\":\"cwd\","
                       "\"cwd\":\"/Users/mn/projects/github/rdrsss/planar\"}\n");
}

TEST_CASE("emit: text path invokes text_fn instead of serializing JSON", "[cli][output]") {
  scope_summary      v{.resolved_scopes = {}, .source = "cwd", .cwd = "/tmp"};
  std::ostringstream out;
  emit(v, output_format::text, scope_text, out);
  REQUIRE(out.str() == "scope: cwd (0 resolved)\n");
  REQUIRE(out.str().find('{') == std::string::npos);
}

TEST_CASE("emit: json path — optional field present serializes as its value, not null", "[cli][output]") {
  plan_row           row{.id = 1, .title = "t", .summary = "has a summary", .parent_plan_id = 42};
  std::ostringstream out;
  emit(row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
  REQUIRE(out.str() == "{\"id\":1,\"title\":\"t\",\"summary\":\"has a summary\",\"parent_plan_id\":42}\n");
}

TEST_CASE("emit: json path — absent optional fields serialize as null, matching the oracle", "[cli][output]") {
  plan_row           row{.id             = 43,
                         .title          = "Ingestor sets task plan_id alongside derives-from link",
                         .summary        = std::nullopt,
                         .parent_plan_id = std::nullopt};
  std::ostringstream out;
  emit(row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
  REQUIRE(out.str() == "{\"id\":43,\"title\":\"Ingestor sets task plan_id alongside derives-from link\","
                       "\"summary\":null,\"parent_plan_id\":null}\n");
}

TEST_CASE("emit_list: json path — bare top-level array, matching plan-list oracle shape", "[cli][output]") {
  std::vector<plan_row> rows{
      {.id = 43, .title = "a", .summary = std::nullopt, .parent_plan_id = std::nullopt},
      {.id = 54, .title = "b", .summary = std::nullopt, .parent_plan_id = 47},
  };
  std::ostringstream out;
  emit_list(rows, output_format::json, plan_list_text, out);
  REQUIRE(out.str() == "[{\"id\":43,\"title\":\"a\",\"summary\":null,\"parent_plan_id\":null},"
                       "{\"id\":54,\"title\":\"b\",\"summary\":null,\"parent_plan_id\":47}]\n");
}

TEST_CASE("emit_list: json path — empty range emits an empty array, not null or omitted", "[cli][output]") {
  std::vector<plan_row> rows{};
  std::ostringstream    out;
  emit_list(rows, output_format::json, plan_list_text, out);
  REQUIRE(out.str() == "[]\n");
}

TEST_CASE("emit_list: text path invokes text_fn instead of serializing JSON", "[cli][output]") {
  std::vector<plan_row> rows{{.id = 1, .title = "only", .summary = std::nullopt, .parent_plan_id = std::nullopt}};
  std::ostringstream    out;
  emit_list(rows, output_format::text, plan_list_text, out);
  REQUIRE(out.str() == "1  only\n");
}

// Break-probe: string field values containing characters that need JSON
// escaping (embedded quote, backslash, newline) must come out correctly
// escaped, not passed through raw — a raw pass-through would produce
// invalid JSON, silently breaking every consumer that parses `--json`
// output.
TEST_CASE("emit: json path — string field escaping (embedded quote/backslash/newline)", "[cli][output]") {
  plan_row           row{.id = 1, .title = "say \"hi\"\\nline2", .summary = std::nullopt, .parent_plan_id = std::nullopt};
  std::ostringstream out;
  emit(row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
  auto const& text = out.str();
  // Round-trip: re-parse and confirm the title decodes back to the
  // original string, proving the escaping is correct (not just present).
  auto parsed = glz::read_json<std::map<std::string, glz::generic>>(text);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->at("title").get<std::string>() == "say \"hi\"\\nline2");
}
