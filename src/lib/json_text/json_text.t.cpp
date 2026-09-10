// @file json_text.t.cpp
// @brief Escape-table tests for `planar.json_text` (plan 996, task 6109).
//
// HOME SAFETY. This file is pure string work. It opens no file, reads no
// environment variable, and constructs no path, so there is nothing here that
// could write outside a scratch root.
//
// ORACLE PROVENANCE. The escape table is not asserted from the C++ source or
// from a JSON spec — it is pinned against bytes the Zig binary actually wrote.
// `local list --json` renders an operator-authored `name` straight out of the
// sandbox link-manifest through `std.json.Stringify`, which makes it a usable
// escaping oracle for arbitrary bytes. Fixture: a hand-written
// `$PLANAR_LOCAL_HOME/.planar/local/skills/.link-manifest.json` whose single
// entry is named
//
//   q"uote\back/slash<TAB>tab<LF>nl<CR>cr<0x08>bs<0x0C>ff<0x01>soh<0x1F>unit<0x7F>del café 日
//
// Run under a redirected home (PLANAR_LOCAL_HOME / PLANAR_HOME / HOME /
// CODEX_HOME all pointing into /tmp), captured with
// `python3 -c "print(repr(open(f,'rb').read()))"` so no shell echo or terminal
// could alter a byte. The oracle emitted, verbatim:
//
//   b'{"Name":"q\\"uote\\\\back/slash\\ttab\\nnl\\rcr\\bbs\\fff\\u0001soh
//     \\u001funit\x7fdel caf\xc3\xa9 \xe6\x97\xa5", ... }\n'
//
// Every assertion below is read off that one capture. Six separable facts fall
// out of it, and each gets its own TEST_CASE so a regression names itself:
//
//   1. `"` -> `\"` and `\` -> `\\`
//   2. TAB/LF/CR/0x08/0x0C -> the five short forms, NOT \u00xx
//   3. 0x01 and 0x1F -> \u0001 / \u001f, LOWERCASE hex
//   4. `/` is NOT escaped        (the oracle shows a bare `/` in `back/slash`)
//   5. 0x7F (DEL) is NOT escaped (the oracle shows a raw \x7f)
//   6. non-ASCII passes through as raw UTF-8, never as \uXXXX

#include <catch2/catch_test_macros.hpp>

import std;
import planar.json_text;

using planar::json_text::append_json_string;
using planar::json_text::json_string;

TEST_CASE("json_text quotes an empty string as two quote characters") {
  REQUIRE(json_string("") == "\"\"");
}

TEST_CASE("json_text passes plain ASCII through unchanged") {
  REQUIRE(json_string("finalize-closeout") == "\"finalize-closeout\"");
}

TEST_CASE("json_text escapes the two mandatory characters") {
  // Oracle: `q"uote\back` came back as `q\"uote\\back`.
  REQUIRE(json_string("q\"uote\\back") == "\"q\\\"uote\\\\back\"");
}

TEST_CASE("json_text uses the five short forms, not \\u00xx") {
  // Oracle: `\ttab\nnl\rcr\bbs\fff` — every one a two-character escape.
  REQUIRE(json_string("\ttab") == "\"\\ttab\"");
  REQUIRE(json_string("\nnl") == "\"\\nnl\"");
  REQUIRE(json_string("\rcr") == "\"\\rcr\"");
  REQUIRE(json_string("\bbs") == "\"\\bbs\"");
  REQUIRE(json_string("\fff") == "\"\\fff\"");
}

TEST_CASE("json_text renders remaining C0 bytes as lowercase \\u00xx") {
  // Oracle: `\u0001soh` and `\u001funit`. Uppercase `\u001F` would still be
  // valid JSON and would still be a parity break, so the case is asserted.
  REQUIRE(json_string("\x01"
                      "soh") == "\"\\u0001soh\"");
  REQUIRE(json_string("\x1F"
                      "unit") == "\"\\u001funit\"");
  REQUIRE(json_string(std::string_view("\0", 1)) == "\"\\u0000\"");
}

TEST_CASE("json_text does NOT escape the forward slash") {
  // Oracle: `back/slash` came back with a bare `/`. This is not cosmetic —
  // every target_path / source_path field the `local` bucket emits is a
  // filesystem path, so escaping `/` would corrupt nearly every record.
  REQUIRE(json_string("/tmp/pb/h/.claude/commands/local-x.md") == "\"/tmp/pb/h/.claude/commands/local-x.md\"");
}

TEST_CASE("json_text does NOT escape DEL (0x7F)") {
  // Oracle: a raw \x7f byte survived. DEL is >= 0x20 so it takes the default
  // arm — the boundary is 0x20, not "is this a control character".
  REQUIRE(json_string("\x7F"
                      "del") == "\"\x7F"
                                "del\"");
}

TEST_CASE("json_text passes non-ASCII through as raw UTF-8") {
  // Oracle: `caf\xc3\xa9 \xe6\x97\xa5` — the bytes are untouched, NOT turned
  // into \u00e9 / \u65e5. The function is byte-oriented and never decodes.
  REQUIRE(json_string("café 日") == "\"café 日\"");
}

TEST_CASE("json_text passes invalid UTF-8 through unchanged") {
  // A consequence of being byte-oriented: a lone continuation byte is not an
  // error and is not replaced. The oracle behaves the same way; nothing in
  // either implementation decodes.
  const auto lone = std::string_view("\xC3", 1);
  REQUIRE(json_string(lone) == "\"\xC3\"");
}

TEST_CASE("json_text reproduces the full oracle capture in one pass") {
  // The whole fixture name, escaped in a single call — this is the assertion
  // that would catch an escape applied in the wrong ORDER (e.g. escaping the
  // backslashes introduced by an earlier escape).
  // NOTE the string splits: `"\x7Fdel"` would lex as ONE hex escape `\x7Fde`
  // (C++ hex escapes are greedy and unbounded), which is out of range for a
  // char. Every literal below therefore ends the escape at a concatenation
  // boundary. Same reason for `"\x01" "soh"` and `"\x1F" "unit"`.
  const std::string input = "q\"uote\\back/slash\ttab\nnl\rcr\bbs\fff\x01"
                            "soh\x1F"
                            "unit\x7F"
                            "del café 日";
  const std::string want  = "\"q\\\"uote\\\\back/slash\\ttab\\nnl\\rcr\\bbs\\fff\\u0001soh\\u001funit\x7F"
                            "del café 日\"";
  REQUIRE(json_string(input) == want);
}

TEST_CASE("json_text appends rather than clearing the destination") {
  // append_json_string is the primitive the renderers actually use, always
  // into a running buffer. A version that cleared `out` would pass every
  // json_string() assertion above and destroy every renderer in the tree.
  std::string out = "{\"name\":";
  append_json_string(out, "bare");
  out.append(",\"kind\":");
  append_json_string(out, "shipped");
  out.push_back('}');
  REQUIRE(out == "{\"name\":\"bare\",\"kind\":\"shipped\"}");
}

// =========================================================================
// Doubles (plan 1006, tasks 6072/6186/6261)
// =========================================================================

TEST_CASE("format_double_fixed never emits an exponent", "[json_text][double]") {
  using planar::json_text::format_double_fixed;

  // The 6261 witness. `std::to_chars`'s default -- which is what
  // `json_dom::format_double` used -- writes `1.375e-06` here.
  CHECK(format_double_fixed(1.375e-06) == "0.000001375");
  CHECK(format_double_fixed(3.0e-7) == "0.0000003");
  CHECK(format_double_fixed(-1.5e-10) == "-0.00000000015");
  CHECK(format_double_fixed(1e-20) == "0." + std::string(19, '0') + "1");

  // Large magnitudes take the other branch of the point shift.
  CHECK(format_double_fixed(1e21) == "1" + std::string(21, '0'));
  CHECK(format_double_fixed(1e300) == "1" + std::string(300, '0'));

  // Shortest-round-trip, not a fixed precision: `1/3` keeps every digit it
  // needs and `3.0` keeps no fractional part at all.
  CHECK(format_double_fixed(1.0 / 3.0) == "0.3333333333333333");
  CHECK(format_double_fixed(3.0) == "3");
  CHECK(format_double_fixed(0.1) == "0.1");
  CHECK(format_double_fixed(-0.0) == "-0");
  CHECK(format_double_fixed(9007199254740992.0) == "9007199254740992");

  // Non-finite returns Zig's bare spellings. NOT JSON -- see the
  // declaration, and append_json_double below.
  CHECK(format_double_fixed(std::numeric_limits<double>::infinity()) == "inf");
  CHECK(format_double_fixed(-std::numeric_limits<double>::infinity()) == "-inf");
  CHECK(format_double_fixed(std::numeric_limits<double>::quiet_NaN()) == "nan");
}

TEST_CASE("append_json_double emits null for every non-finite value", "[json_text][double]") {
  using planar::json_text::json_double;

  // The 6186 witness at the emitter. A bare `inf` is what `models evals
  // --quality-floor inf` used to write into `gates`, and no JSON parser
  // accepts it. `nan` was QUOTED there, which is valid JSON but a
  // different TYPE from every other value the field can hold -- the
  // asymmetry nobody would guess. Both are `null` now.
  CHECK(json_double(std::numeric_limits<double>::infinity()) == "null");
  CHECK(json_double(-std::numeric_limits<double>::infinity()) == "null");
  CHECK(json_double(std::numeric_limits<double>::quiet_NaN()) == "null");
  CHECK(json_double(-std::numeric_limits<double>::quiet_NaN()) == "null");

  // Finite values are unaffected and carry format_double_fixed's spelling.
  CHECK(json_double(0.5) == "0.5");
  CHECK(json_double(1.375e-06) == "0.000001375");
}
