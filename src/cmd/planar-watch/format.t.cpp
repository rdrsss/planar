// @file format.t.cpp
// @brief Unit tests for `planar.cmd.planar_watch.handlers.format` — the
// column formatters behind `ps` and `tree` (plan 996, task 6120).
//
// WHY THESE ARE A SEPARATE FILE from `handlers.t.cpp`. The formatters are
// pure functions over strings, and their interesting inputs — a summary
// whose 77th BYTE falls inside a multi-byte character, a worktree path one
// character either side of forty, a heartbeat exactly at a bucket boundary
// — are inputs a fixture-driven handler test cannot produce precisely.
// Driving them through a database and a verb would mean asserting on the
// interesting byte through two layers of incidental context.
//
// It is also where a survivor was found. The UTF-8 walk-back in
// `render_activity_summary` was covered by NOTHING before this file
// existed: deleting the loop left every handler test, every parity case
// and the ad-hoc differential all green, because none of their summaries
// happened to put a continuation byte at offset 77. The regime table below
// is chosen so each branch has at least one input that only it answers
// correctly.
//
// ORACLE PROVENANCE. Every expectation is a transcription of bytes
// `zig/zig-out/bin/planar-watch ps` wrote for a claim whose latest action
// carried the stated summary, under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/PLANAR_LOCAL_HOME. The four
// captures:
//
//   ''                        -> activity:""
//   'a' * 80                  -> activity:"aaaa…aaaa"   (80 a's, NO ellipsis)
//   'b' * 81                  -> activity:"bbbb…bbbb…"  (77 b's + U+2026)
//   'the coordination layer rewired every caller and then some more words
//    ——— tail padding to push past eighty bytes'
//                             -> activity:"the coordination layer rewired
//                                every caller and then some more words ——…"
//
// The last one is the load-bearing capture: byte 77 of that string is 0x94,
// a continuation byte in the middle of the THIRD em dash, so a cut at 77
// would emit a truncated code point. The oracle backs up to 75 and keeps
// two em dashes. Byte-for-byte agreement on THAT is the property this file
// exists to hold.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cmd.planar_watch.handlers.format;

namespace {

namespace fmt = planar::cmd::watch::handlers::format;

/// @brief `render_activity_summary` takes an optional; most cases have one.
/// @param text The summary.
/// @return The rendered column body.
auto activity(std::string_view text) -> std::string {
  return fmt::render_activity_summary(std::optional<std::string>{std::string{text}});
}

} // namespace

TEST_CASE("render_activity_summary never splits a multi-byte character", "[cmd][watch][format]") {
  // THE SURVIVOR CASE. With the continuation-byte walk-back removed, the
  // cut lands at byte 77 — 0x94, the third byte of the third em dash — and
  // the column emits an incomplete code point into the operator's terminal.
  // Every other summary in this repo's fixtures happens to have an ASCII
  // byte at 77, which is why nothing else catches it.
  constexpr std::string_view straddling = "the coordination layer rewired every caller and then some more words "
                                          "\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94 tail padding to push past eighty bytes";
  REQUIRE(straddling.size() > 80);
  REQUIRE((static_cast<unsigned char>(straddling[77]) & 0xC0U) == 0x80U);

  auto const rendered = activity(straddling);
  CHECK(rendered == "\"the coordination layer rewired every caller and then some more words "
                    "\xe2\x80\x94\xe2\x80\x94\xe2\x80\xa6\"");
  // Stated independently of the literal above, so a typo in the expectation
  // cannot make both halves agree on the wrong answer: the body is 75 bytes
  // of the original plus the 3-byte ellipsis, and it is valid UTF-8 — the
  // last character is complete.
  CHECK(rendered.size() == 75 + 3 + 2);
  CHECK(rendered.ends_with("\xe2\x80\xa6\""));
}

TEST_CASE("render_activity_summary's four regimes", "[cmd][watch][format]") {
  // Absent and present-but-empty render identically, and BOTH are quoted —
  // the column key `activity:` is always followed by a quoted body, so a
  // consumer splitting on `"` sees a consistent shape.
  CHECK(fmt::render_activity_summary(std::nullopt) == "\"\"");
  CHECK(activity("") == "\"\"");

  CHECK(activity("done") == "\"done\"");

  // EXACTLY at the limit is NOT truncated. Off-by-one either way here is
  // silently wrong output, and the two cases differ by a single byte.
  auto const eighty = std::string(80, 'a');
  CHECK(activity(eighty) == "\"" + eighty + "\"");

  auto const eighty_one = std::string(81, 'b');
  CHECK(activity(eighty_one) == "\"" + std::string(77, 'b') + "\xe2\x80\xa6\"");
  // The whole rendered column, quotes included, stays within 82 bytes so a
  // table of them lines up.
  CHECK(activity(eighty_one).size() == 82);
  CHECK(activity(std::string(5000, 'c')).size() == 82);
}

TEST_CASE("render_worktree_column elides on the FULL path length, not the basename's", "[cmd][watch][format]") {
  CHECK(fmt::render_worktree_column(std::nullopt) == "\"\"");
  CHECK(fmt::render_worktree_column(std::optional<std::string>{""}) == "\"\"");

  // A short path renders as its basename ALONE and UNQUOTED — note the
  // asymmetry with the empty case above, which is quoted. That is the
  // reference binary's, and a port that quoted both would look tidier and
  // be wrong.
  CHECK(fmt::render_worktree_column(std::optional<std::string>{"/tmp/wt/agent-1"}) == "agent-1");
  CHECK(fmt::render_worktree_column(std::optional<std::string>{"agent-1"}) == "agent-1");

  // The 40-character test is against the FULL path. These two have the SAME
  // basename and differ only in the length of the prefix, so they separate
  // "measures the path" from "measures the basename" — the mistake worth
  // catching, since the basename is what gets printed either way.
  auto const short_path = std::string("/") + std::string(30, 'd') + "/agent-1"; // 39
  REQUIRE(short_path.size() <= 40);
  CHECK(fmt::render_worktree_column(std::optional<std::string>{short_path}) == "agent-1");

  auto const long_path = std::string("/") + std::string(60, 'd') + "/agent-1";
  REQUIRE(long_path.size() > 40);
  CHECK(fmt::render_worktree_column(std::optional<std::string>{long_path}) == "\xe2\x80\xa6"
                                                                              "agent-1");
}

TEST_CASE("relative_time's buckets, at their boundaries", "[cmd][watch][format]") {
  // A fixed anchor: 2026-05-29T12:00:00.000Z.
  constexpr std::int64_t anchor = 1'780'056'000'000;

  // Under five seconds is "just now"…
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:59:57.000Z") == "just now");
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:59:55.001Z") == "just now");
  // …and so is a FUTURE timestamp, which is why the delta is signed. Two
  // machines sharing a database can disagree about the clock, and a viewer
  // that rendered `-3s ago` would be reporting the skew as data.
  CHECK(fmt::relative_time(anchor, "2026-05-29T12:00:30.000Z") == "just now");

  // Each boundary is checked from BOTH sides; a `<` written as `<=`
  // survives a one-sided check.
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:59:55.000Z") == "5s ago");
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:59:45.000Z") == "15s ago");
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:59:01.000Z") == "59s ago");
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:59:00.000Z") == "1m ago");
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:01:00.000Z") == "59m ago");
  CHECK(fmt::relative_time(anchor, "2026-05-29T11:00:00.000Z") == "1h ago");
  CHECK(fmt::relative_time(anchor, "2026-05-28T13:00:00.000Z") == "23h ago");
  CHECK(fmt::relative_time(anchor, "2026-05-28T12:00:00.000Z") == "1d ago");
  CHECK(fmt::relative_time(anchor, "2026-04-29T12:00:00.000Z") == "30d ago");

  // The `last_hb:` column strips the suffix the key already implies…
  CHECK(fmt::render_relative_heartbeat(anchor, "2026-05-29T11:59:45.000Z") == "15s");
  // …but "just now" has no " ago" to strip and survives whole.
  CHECK(fmt::render_relative_heartbeat(anchor, "2026-05-29T11:59:59.000Z") == "just now");
  // An unparseable or empty timestamp empties the VALUE, keeping the key.
  CHECK(fmt::render_relative_heartbeat(anchor, "").empty());
  CHECK(fmt::render_relative_heartbeat(anchor, "not-a-timestamp").empty());
}

TEST_CASE("iso_to_ms accepts both stored widths and refuses everything else", "[cmd][watch][format]") {
  // Both widths occur in the tables: `strftime('%Y-%m-%dT%H:%M:%fZ')` writes
  // milliseconds, but older rows and hand-written fixtures carry seconds.
  CHECK(fmt::iso_to_ms("1970-01-01T00:00:00.000Z") == 0);
  CHECK(fmt::iso_to_ms("1970-01-01T00:00:00Z") == 0);
  CHECK(fmt::iso_to_ms("1970-01-01T00:00:01.500Z") == 1500);
  CHECK(fmt::iso_to_ms("2026-05-29T12:00:00.000Z") == 1'780'056'000'000);
  // Leap day, because the date arithmetic is the part most likely to be
  // hand-rolled wrong and a February 29th is where that shows.
  CHECK(fmt::iso_to_ms("2024-02-29T00:00:00.000Z") == 1'709'164'800'000);

  // Refusals are unset, never a guess. A timestamp this function guessed at
  // would surface as a plausible but wrong `last_hb:` column, which is
  // worse than a blank one.
  CHECK_FALSE(fmt::iso_to_ms("").has_value());
  CHECK_FALSE(fmt::iso_to_ms("2026-05-29").has_value());
  CHECK_FALSE(fmt::iso_to_ms("2026-05-29T12:00:00").has_value());      // no Z
  CHECK_FALSE(fmt::iso_to_ms("2026-05-29 12:00:00Z").has_value());     // no T
  CHECK_FALSE(fmt::iso_to_ms("2026-05-29T12:00:00.00Z").has_value());  // 2-digit millis
  CHECK_FALSE(fmt::iso_to_ms("2026-05-29T12:00:00,000Z").has_value()); // comma
  CHECK_FALSE(fmt::iso_to_ms("2026-13-01T00:00:00.000Z").has_value()); // month 13
  CHECK_FALSE(fmt::iso_to_ms("2026-02-30T00:00:00.000Z").has_value()); // no such day
  CHECK_FALSE(fmt::iso_to_ms("20xx-05-29T12:00:00.000Z").has_value()); // non-digit
}

TEST_CASE("now_iso matches the shape of the timestamps it is compared against", "[cmd][watch][format]") {
  // `generated_at` is not oracle-comparable — it is read from the host
  // clock at emit time — so what is pinned is its SHAPE. It must be the
  // same fixed-width form SQLite writes, because the whole reason it is
  // rendered this way is so a consumer can string-compare it against a
  // `claimed_at` without a date parser.
  auto const now = fmt::now_iso();
  CHECK(now.size() == 24);
  CHECK(now[4] == '-');
  CHECK(now[7] == '-');
  CHECK(now[10] == 'T');
  CHECK(now[13] == ':');
  CHECK(now[16] == ':');
  CHECK(now[19] == '.');
  CHECK(now.back() == 'Z');
  // Round-trips through this module's own parser — which also means the
  // two halves of the clock agree with each other.
  auto const parsed = fmt::iso_to_ms(now);
  REQUIRE(parsed.has_value());
  CHECK(std::abs(*parsed - fmt::now_ms()) < 5000);
}
