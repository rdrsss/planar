// @file json_dom.t.cpp
// @brief Parser and writer tests for `planar.json_dom` (plan 996, task 6190).
//
// HOME SAFETY. Pure string work. This file opens no file, reads no
// environment variable and constructs no path.
//
// ORACLE PROVENANCE. Nothing here is asserted from the C++ source or from
// the JSON spec. Every expectation is read off bytes the Zig binary
// actually wrote, captured through `templates render` and `templates show`
// under a pinned scratch arena (PLANAR_DB / PLANAR_HOME /
// PLANAR_CONFIG_PATH / PLANAR_LOCAL_HOME / HOME all redirected into /tmp),
// with stderr taken through a PIPE rather than a file redirect — see
// src/cmd/parity_harness.hpp's header for why a `2> file` capture silently
// corrupts multi-write Zig output.
//
// `templates render` is a usable oracle for this module specifically
// because it round-trips: it decodes an operator-authored template with
// `std.json.parseFromSlice` and re-encodes the result with
// `std.json.Stringify(.{ .whitespace = .indent_2 })`. Hand a probe template
// in, read the exact bytes out.
//
// THE FOUR CAPTURES THAT DROVE THIS FILE. Every one of them contradicted
// the first draft of json_dom.cpp, and each has its own TEST_CASE below so
// a regression names itself:
//
//   1. KEY ORDER IS INSERTION ORDER.
//      Probe: templates/defaults/github-issues/issue.json, authored
//        title, body, labels, assignees
//      Oracle emitted those four keys in that order — NOT sorted. A
//      `std::map`-backed DOM (glz::json_t) yields assignees, body, labels,
//      title: valid JSON, same values, different bytes, exit 0.
//
//   2. DUPLICATE KEYS ARE A PARSE ERROR.
//      Probe: {"dup":"a","dup":"b","nest":{...}}
//      Oracle: exit 1, `error: template probe/px/dup not found` — the
//      candidate fails to parse, so it never wins its resolution level and
//      the whole chain falls through. The draft kept the last value and
//      rendered it at exit 0.
//
//   3. AN INTEGER TOO LARGE FOR i64 KEEPS ITS SOURCE TEXT.
//      Probe: {"big":12345678901234567890}
//      Oracle: `"big": 12345678901234567890` exactly. The draft folded it
//      into a double and emitted 12345678901234567168.
//
//   4. A FINITE FLOAT IS REFORMATTED.
//      Probe: {"f":1.50}
//      Oracle: `"f": 1.5`. So (3) cannot be fixed by "keep every number's
//      raw text" — the two arms genuinely differ.
//
// The empty-container and escape captures come from the same probe set;
// each names its own bytes inline.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.json_dom;

using planar::json_dom::json_kind;
using planar::json_dom::json_value;
using planar::json_dom::parse_json;
using planar::json_dom::stringify_indent2;

namespace {

/// @brief Parse then re-encode, the way `templates render` does.
/// @param text The document.
/// @return The re-encoded document, or `<PARSE-ERROR>` when it did not
/// parse — a sentinel rather than an exception so a test that expected a
/// round trip fails with a readable diff instead of a crash.
auto round_trip(std::string_view text) -> std::string {
  auto parsed = parse_json(text);
  if (!parsed.has_value()) {
    return "<PARSE-ERROR>";
  }
  return stringify_indent2(*parsed);
}

} // namespace

// --- capture 1: key order ---------------------------------------------

TEST_CASE("json_dom preserves object key order rather than sorting it", "[json_dom][order]") {
  // The oracle's own `templates show default github-issues issue` bytes,
  // reduced to the key sequence. Sorted order would be assignees, body,
  // labels, title — the exact reverse of what matters here.
  auto const parsed = parse_json(R"({"title":"t","body":"b","labels":[],"assignees":[]})");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->kind == json_kind::object);
  REQUIRE(parsed->object.size() == 4);
  CHECK(parsed->object[0].first == "title");
  CHECK(parsed->object[1].first == "body");
  CHECK(parsed->object[2].first == "labels");
  CHECK(parsed->object[3].first == "assignees");
}

TEST_CASE("json_dom round-trips a non-lexicographic key order byte for byte", "[json_dom][order]") {
  // Oracle bytes from `templates render default github-issues issue
  // --entity task:1`, with the substituted values inlined.
  auto const expected = "{\n"
                        "  \"title\": \"Do the thing\",\n"
                        "  \"body\": \"acceptance\",\n"
                        "  \"labels\": [\n"
                        "    \"planar-managed\",\n"
                        "    \"type:task\"\n"
                        "  ],\n"
                        "  \"assignees\": []\n"
                        "}";
  CHECK(round_trip(R"({"title":"Do the thing","body":"acceptance",)"
                   R"("labels":["planar-managed","type:task"],"assignees":[]})") == expected);
}

// --- capture 2: duplicate keys ----------------------------------------

TEST_CASE("json_dom REJECTS duplicate object keys", "[json_dom][duplicate]") {
  // `std.json.ParseOptions.duplicate_field_behavior` defaults to
  // `.@"error"`. Oracle: `templates show probe/px/dup` is exit 1
  // `template probe/px/dup not found`, because the file never parses.
  CHECK_FALSE(parse_json(R"({"dup":"a","dup":"b"})").has_value());
  // Nested, and not adjacent — the check is per-object, not per-document.
  CHECK_FALSE(parse_json(R"({"o":{"k":1,"j":2,"k":3}})").has_value());
  // Distinct keys that merely share a prefix are fine.
  CHECK(parse_json(R"({"k":1,"kk":2})").has_value());
}

TEST_CASE("json_dom does not confuse a repeated key with a repeated VALUE", "[json_dom][duplicate]") {
  // The failure mode a naive `any_of` over values would produce.
  CHECK(round_trip(R"({"a":"same","b":"same"})") == "{\n  \"a\": \"same\",\n  \"b\": \"same\"\n}");
}

// --- captures 3 and 4: the three number arms --------------------------

TEST_CASE("json_dom keeps an integer that fits i64 exact", "[json_dom][number]") {
  CHECK(round_trip("{\"n\":42}") == "{\n  \"n\": 42\n}");
  CHECK(round_trip("{\"n\":-42}") == "{\n  \"n\": -42\n}");
  CHECK(round_trip("{\"n\":0}") == "{\n  \"n\": 0\n}");
  // The i64 boundaries themselves, which must NOT tip into the raw arm.
  CHECK(round_trip("{\"n\":9223372036854775807}") == "{\n  \"n\": 9223372036854775807\n}");
  CHECK(round_trip("{\"n\":-9223372036854775808}") == "{\n  \"n\": -9223372036854775808\n}");
}

TEST_CASE("json_dom keeps an integer too large for i64 as SOURCE TEXT", "[json_dom][number]") {
  // Oracle: `"big": 12345678901234567890`. Folding this into a double
  // emits 12345678901234567168 — silent precision loss in a payload that
  // then gets pushed to Jira.
  auto const parsed = parse_json("{\"big\":12345678901234567890}");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->object.size() == 1);
  CHECK(parsed->object[0].second.kind == json_kind::number_raw);
  CHECK(round_trip("{\"big\":12345678901234567890}") == "{\n  \"big\": 12345678901234567890\n}");
  // One past i64::max, so the boundary case above and this one bracket it.
  CHECK(round_trip("{\"big\":9223372036854775808}") == "{\n  \"big\": 9223372036854775808\n}");
}

TEST_CASE("json_dom REFORMATS a finite float rather than keeping its text", "[json_dom][number]") {
  // Oracle: `1.50` came back as `1.5`. This is why capture 3 cannot be
  // fixed by keeping every number's raw text.
  CHECK(round_trip("{\"f\":1.50}") == "{\n  \"f\": 1.5\n}");
  CHECK(round_trip("{\"f\":1.0}") == "{\n  \"f\": 1\n}");
}

TEST_CASE("json_dom keeps a float that overflows to infinity as source text", "[json_dom][number]") {
  // zig's `parseFromNumberSlice` checks `isFinite` and falls back to
  // `.number_string`, because `inf` is not emittable JSON.
  auto const parsed = parse_json("{\"f\":1e400}");
  REQUIRE(parsed.has_value());
  CHECK(parsed->object[0].second.kind == json_kind::number_raw);
  CHECK(round_trip("{\"f\":1e400}") == "{\n  \"f\": 1e400\n}");
}

// --- the writer's whitespace state machine ----------------------------

TEST_CASE("json_dom collapses an EMPTY container inline", "[json_dom][whitespace]") {
  // Oracle: `"assignees": []` sits on one line in the same document where
  // `"labels"` breaks across four. Same for `{}`.
  CHECK(round_trip("{\"e\":{},\"a\":[]}") == "{\n  \"e\": {},\n  \"a\": []\n}");
  CHECK(round_trip("{}") == "{}");
  CHECK(round_trip("[]") == "[]");
}

TEST_CASE("json_dom breaks a NON-empty container across lines", "[json_dom][whitespace]") {
  CHECK(round_trip("[1]") == "[\n  1\n]");
  CHECK(round_trip("[1,2]") == "[\n  1,\n  2\n]");
}

TEST_CASE("json_dom indents two spaces per nesting level", "[json_dom][whitespace]") {
  // Oracle bytes from `templates render default jira epic --entity plan:1`,
  // whose `fields.issuetype.name` is three levels deep.
  auto const expected = "{\n"
                        "  \"fields\": {\n"
                        "    \"issuetype\": {\n"
                        "      \"name\": \"Epic\"\n"
                        "    },\n"
                        "    \"labels\": [\n"
                        "      \"planar-managed\"\n"
                        "    ]\n"
                        "  }\n"
                        "}";
  CHECK(round_trip(R"({"fields":{"issuetype":{"name":"Epic"},"labels":["planar-managed"]}})") == expected);
}

TEST_CASE("json_dom emits no trailing newline", "[json_dom][whitespace]") {
  // The caller appends it. See the stdout-terminator contract.
  auto const out = round_trip("{\"a\":1}");
  REQUIRE_FALSE(out.empty());
  CHECK(out.back() == '}');
}

TEST_CASE("json_dom writes the scalar literals the way zig does", "[json_dom][whitespace]") {
  CHECK(round_trip("{\"t\":true,\"f\":false,\"z\":null}") == "{\n  \"t\": true,\n  \"f\": false,\n  \"z\": null\n}");
}

// --- the escape table -------------------------------------------------

TEST_CASE("json_dom uses the short escape forms including \\b and \\f", "[json_dom][escape]") {
  // Oracle: `templates render probe px types` emitted `"s": "tab\there"`.
  // A first draft claimed 0x08/0x0C had NO short forms; zig's
  // `outputSpecialEscape` gives them `\b` and `\f`.
  CHECK(round_trip("{\"s\":\"a\\tb\"}") == "{\n  \"s\": \"a\\tb\"\n}");
  CHECK(round_trip("{\"s\":\"a\\bb\"}") == "{\n  \"s\": \"a\\bb\"\n}");
  CHECK(round_trip("{\"s\":\"a\\fb\"}") == "{\n  \"s\": \"a\\fb\"\n}");
  CHECK(round_trip("{\"s\":\"a\\nb\"}") == "{\n  \"s\": \"a\\nb\"\n}");
  CHECK(round_trip("{\"s\":\"a\\rb\"}") == "{\n  \"s\": \"a\\rb\"\n}");
}

TEST_CASE("json_dom escapes other control bytes as LOWERCASE \\u00xx", "[json_dom][escape]") {
  CHECK(round_trip("{\"s\":\"\\u0001\"}") == "{\n  \"s\": \"\\u0001\"\n}");
  CHECK(round_trip("{\"s\":\"\\u001F\"}") == "{\n  \"s\": \"\\u001f\"\n}");
}

TEST_CASE("json_dom passes DEL and UTF-8 through unescaped", "[json_dom][escape]") {
  // `escape_unicode` defaults to false in std.json and nothing sets it.
  CHECK(round_trip("{\"s\":\"caf\xc3\xa9\"}") == "{\n  \"s\": \"caf\xc3\xa9\"\n}");
  CHECK(round_trip("{\"s\":\"\\u007f\"}") == "{\n  \"s\": \"\x7f\"\n}");
}

TEST_CASE("json_dom decodes a surrogate pair into one UTF-8 code point", "[json_dom][escape]") {
  // U+1F600. Re-encoded raw, because non-ASCII is not escaped on output.
  CHECK(round_trip("{\"s\":\"\\ud83d\\ude00\"}") == "{\n  \"s\": \"\xf0\x9f\x98\x80\"\n}");
}

// --- strictness -------------------------------------------------------

TEST_CASE("json_dom rejects trailing content after the top-level value", "[json_dom][strict]") {
  // `std.json.parseFromSlice` requires the document to be exhausted;
  // `glz::read_json` does not, which was task 6086's finding.
  CHECK_FALSE(parse_json(R"({"a":1}garbage)").has_value());
  CHECK_FALSE(parse_json(R"({"a":1} {"b":2})").has_value());
  // Trailing WHITESPACE is fine.
  CHECK(parse_json("{\"a\":1}  \n\t").has_value());
}

TEST_CASE("json_dom rejects the usual JSON5-isms", "[json_dom][strict]") {
  CHECK_FALSE(parse_json(R"({"a":1,})").has_value());              // trailing comma
  CHECK_FALSE(parse_json(R"({a:1})").has_value());                 // unquoted key
  CHECK_FALSE(parse_json(R"({"a":'x'})").has_value());             // single quotes
  CHECK_FALSE(parse_json(R"({"a":1} // c)").has_value());          // comment
  CHECK_FALSE(parse_json(R"({"a":NaN})").has_value());             // NaN
  CHECK_FALSE(parse_json(R"({"a":01})").has_value());              // leading zero
  CHECK_FALSE(parse_json(R"({"a":.5})").has_value());              // no integer part
  CHECK_FALSE(parse_json(R"({"a":1.})").has_value());              // no fraction digits
  CHECK_FALSE(parse_json("").has_value());                         // empty document
  CHECK_FALSE(parse_json("{\"a\":\"raw\ncontrol\"}").has_value()); // unescaped control byte
}

TEST_CASE("json_dom refuses nesting past k_max_depth instead of overflowing the stack", "[json_dom][strict]") {
  // A documented divergence: the oracle's Value parser uses a heap stack
  // and would accept this. A loud refusal beats a crash, and no real
  // template nests past three. See json_dom.cppm's `k_max_depth`.
  auto const deep = std::string(planar::json_dom::k_max_depth + 5, '[');
  CHECK_FALSE(parse_json(deep).has_value());

  // Well within the limit still parses, so the guard is not off by orders
  // of magnitude.
  std::string ok = std::string(64, '[') + std::string(64, ']');
  CHECK(parse_json(ok).has_value());
}

TEST_CASE("json_dom::find returns nullptr rather than a default for a missing key", "[json_dom]") {
  auto const parsed = parse_json(R"({"a":1})");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->find("a") != nullptr);
  CHECK(parsed->find("b") == nullptr);
  // Not an object at all.
  auto const scalar = parse_json("1");
  REQUIRE(scalar.has_value());
  CHECK(scalar->find("a") == nullptr);
}
