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
// escaping (embedded quote, backslash, a REAL newline byte — not the
// two-character sequence backslash-n, which is already valid JSON content
// and proves nothing about escaping) must come out correctly escaped, not
// passed through raw — a raw pass-through would produce invalid JSON,
// silently breaking every consumer that parses `--json` output.
//
// B2 (M2 boundary review, plan 996 task 6066): the ORIGINAL version of this
// test used the C++ string literal "say \"hi\"\\nline2", which is a
// backslash followed by the letter 'n' — two ordinary, already-valid-JSON
// characters, not the single 0x0A newline byte the test's own name claimed
// to cover. No byte outside Glaze's 7-entry short-escape table ever
// appeared in the input, so the test could not have caught B1 (Glaze
// passing 0x00-0x1F through raw by default) even though the escaping WAS
// broken at the time. Fixed below to use a real embedded newline (0x0A,
// via '\n' as a value, not "\\n" as two literal chars) plus a dedicated
// sweep over every 0x00-0x1F byte including 0x1B (ESC), matching the
// task's required 0x1B citation.
TEST_CASE("emit: json path — string field escaping (embedded quote/backslash/real newline)", "[cli][output]") {
  plan_row row{
      .id = 1, .title = "say \"hi\"" + std::string(1, '\n') + "line2", .summary = std::nullopt, .parent_plan_id = std::nullopt};
  REQUIRE(row.title.find('\n') != std::string::npos); // guard: the byte under test is really in the fixture
  std::ostringstream out;
  emit(row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
  auto const& text = out.str();
  // `emit` itself appends exactly one trailing '\n' after the JSON payload
  // (see output.cppm: `out << *result << '\n';`) — that terminator is not
  // part of the escaped payload under test, so strip it before checking
  // for a raw, un-escaped newline byte anywhere in the JSON text itself.
  REQUIRE(text.back() == '\n');
  auto const& payload = text.substr(0, text.size() - 1);
  // The raw byte must never appear un-escaped in the wire text — this is
  // the assertion that actually fails against the pre-fix behavior (raw
  // passthrough), unlike a round-trip-only check which a lenient/lax JSON
  // reader could paper over.
  CHECK(payload.find('\n') == std::string::npos);
  CHECK(payload.find("\\n") != std::string::npos);
  // Round-trip: re-parse and confirm the title decodes back to the
  // original string, proving the escaping is correct (not just present).
  auto parsed = glz::read_json<std::map<std::string, glz::generic>>(text);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->at("title").get<std::string>() == row.title);
}

// Break-probe: every byte in 0x00-0x1F, including the 7 Glaze already
// short-escapes AND the ones B1 found passed through raw (e.g. 0x01, and
// 0x1B/ESC specifically named by the task brief). Before the
// `escape_control_characters` fix, every byte NOT in Glaze's 7-entry table
// (i.e. every one of these except \b \t \n \f \r) was written raw,
// producing an unparseable document — `glz::read_json` below is the
// non-vacuous half of the probe: it FAILS on the pre-fix output for those
// bytes, proving this isn't just a "the option is set" tautology.
TEST_CASE("emit: json path — every 0x00-0x1F control byte round-trips through valid JSON, including 0x1B",
          "[cli][output][break-probe]") {
  for (int b = 0x00; b <= 0x1F; ++b) {
    INFO("control byte 0x" << std::hex << b);
    plan_row           row{.id             = 1,
                           .title          = "pre" + std::string(1, static_cast<char>(b)) + "post",
                           .summary        = std::nullopt,
                           .parent_plan_id = std::nullopt};
    std::ostringstream out;
    emit(row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
    auto const& text = out.str();
    REQUIRE(text.back() == '\n'); // emit()'s own trailing terminator, not part of the escaped payload
    auto const& payload = text.substr(0, text.size() - 1);
    CHECK(payload.find(static_cast<char>(b)) == std::string::npos); // never appears raw
    auto parsed = glz::read_json<std::map<std::string, glz::generic>>(text);
    REQUIRE(parsed.has_value()); // must be valid, parseable JSON for every byte
    CHECK(parsed->at("title").get<std::string>() == row.title);
  }
  // 0x1B (ESC) called out explicitly by the task brief.
  plan_row esc_row{
      .id = 1, .title = "esc[" + std::string(1, '\x1b') + "]seq", .summary = std::nullopt, .parent_plan_id = std::nullopt};
  std::ostringstream out;
  emit(esc_row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
  auto const& text = out.str();
  CHECK(text.find('\x1b') == std::string::npos);
  CHECK(text.find("\\u001b") != std::string::npos); // lowercase hex, matching the Zig oracle (json/Stringify.zig:642)
  auto parsed = glz::read_json<std::map<std::string, glz::generic>>(text);
  REQUIRE(parsed.has_value());
  CHECK(parsed->at("title").get<std::string>() == esc_row.title);
}

// Break-probe for B1's hex-case fix specifically: a control byte with no
// short escape (e.g. 0x01) must render as LOWERCASE hex (\u0001), matching
// Zig's std.json.Stringify.value (json/Stringify.zig:642,
// `printInt(codepoint, 16, .lower, ...)`), not Glaze's own uppercase
// default (json/write.hpp:811/922, "0123456789ABCDEF"). This is the
// non-vacuous proof that `detail::lowercase_control_escapes` actually ran:
// asserting only "text.find(\"\\u0001\")" would pass whether the hex came
// out upper or lower (0-9 and 1 have no case), so the fixture below
// deliberately picks a byte (0x0B) whose hex digit (B) DOES have a case,
// so an uppercase regression fails this check.
TEST_CASE("emit: json path — control-char hex escapes are lowercase, matching the Zig oracle", "[cli][output][break-probe]") {
  plan_row row{.id = 1, .title = "x" + std::string(1, '\x0b') + "y", .summary = std::nullopt, .parent_plan_id = std::nullopt};
  std::ostringstream out;
  emit(row, output_format::json, [](plan_row const&, std::ostream&) {}, out);
  auto const& text = out.str();
  CHECK(text.find("\\u000b") != std::string::npos);
  CHECK(text.find("\\u000B") == std::string::npos);
}
