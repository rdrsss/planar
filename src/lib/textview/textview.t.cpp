// textview.t.cpp: `planar.textview` (plan 1080, task 7082). The expectations
// come from the contract in textview.cppm (which characters are hazards, the
// width approximation, where a value is cut), not from the implementation's
// tables. Pure string work: no file, environment variable or process.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.textview;

namespace tv = planar::textview;

TEST_CASE("textview: every listed format character is a hazard and an ordinary one is not",
          "[lib][textview][hq-view-escape-fields]") {
  for (char32_t cp : {U'\u200b', U'\u200c', U'\u200d', U'\u200e', U'\u200f', U'\u202a', U'\u202b', U'\u202c', U'\u202d',
                      U'\u202e', U'\u2066', U'\u2067', U'\u2068', U'\u2069', U'\ufeff', U'\u2028', U'\u2029'}) {
    INFO("code point: " << static_cast<std::uint32_t>(cp));
    CHECK(tv::is_format_char(cp));
  }
  for (char32_t cp : {U'a', U' ', U'\u00e9', U'\u65e5', U'\u2026', U'\u202f', U'\u2065', U'\u200a', U'\uff00'}) {
    INFO("code point: " << static_cast<std::uint32_t>(cp));
    CHECK_FALSE(tv::is_format_char(cp));
  }
}

TEST_CASE("textview: has_hazard flags controls, C1, format characters and invalid UTF-8 only",
          "[lib][textview][hq-view-escape-fields]") {
  CHECK_FALSE(tv::has_hazard(""));
  CHECK_FALSE(tv::has_hazard("plain text /path -x \"q\" \\b"));
  CHECK_FALSE(tv::has_hazard("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xE2\x80\xA6"));
  CHECK(tv::has_hazard("a\nb"));
  CHECK(tv::has_hazard("a\x7f"));
  CHECK(tv::has_hazard("a\xC2\x9b"
                       "31m"));
  CHECK(tv::has_hazard("a\xE2\x80\xAE"
                       "b"));
  CHECK(tv::has_hazard("a\xE2\x80\xA8"
                       "b"));
  CHECK(tv::has_hazard("\xEF\xBB\xBF"
                       "x"));
  CHECK(tv::has_hazard("a\xFF"
                       "b"));
  CHECK(tv::has_hazard("a\xE2\x80")); // truncated sequence
}

TEST_CASE("textview: quote_text escapes the quote, backslash, controls and format characters",
          "[lib][textview][hq-view-escape-fields]") {
  CHECK(tv::quote_text("") == "\"\"");
  CHECK(tv::quote_text("plain") == "\"plain\"");
  CHECK(tv::quote_text("a\"b\\c") == "\"a\\\"b\\\\c\"");
  CHECK(tv::quote_text("a\nb\rc\td") == "\"a\\nb\\rc\\td\"");
  CHECK(tv::quote_text("\x1b[2J\x7f") == "\"\\u001b[2J\\u007f\"");
  CHECK(tv::quote_text("\xC2\x9b") == "\"\\u009b\"");
  CHECK(tv::quote_text("x\xE2\x80\xAE"
                       "y") == "\"x\\u202ey\"");
  CHECK(tv::quote_text("\xE2\x80\x8B\xE2\x80\xA8\xE2\x80\xA9\xEF\xBB\xBF") == "\"\\u200b\\u2028\\u2029\\ufeff\"");
  // Ordinary non-ASCII text passes through.
  CHECK(tv::quote_text("caf\xC3\xA9 \xE6\x97\xA5") == "\"caf\xC3\xA9 \xE6\x97\xA5\"");
  // A byte that is not UTF-8 is shown, not passed on.
  CHECK(tv::quote_text("a\xFF"
                       "b") == "\"a\\xffb\"");
}

TEST_CASE("textview: display_width counts code points, wide characters twice and format characters not at all",
          "[lib][textview][hq-view-escape-fields]") {
  CHECK(tv::display_width("") == 0);
  CHECK(tv::display_width("abcd") == 4);
  CHECK(tv::display_width("caf\xC3\xA9") == 4);              // five bytes, four columns
  CHECK(tv::display_width("\xE6\x97\xA5\xE6\x9C\xAC") == 4); // two wide characters
  CHECK(tv::display_width("\xEF\xBC\xA1") == 2);             // fullwidth A
  CHECK(tv::display_width("e\xCC\x81") == 1);                // e + combining acute
  CHECK(tv::display_width("a\xE2\x80\x8B"
                          "b") == 2);            // zero width space
  CHECK(tv::display_width("\xE2\x80\xA6") == 1); // the marker
  CHECK(tv::display_width("\xFF\xFE") == 2);     // invalid bytes: one each
}

TEST_CASE("textview: truncate_display keeps what fits, marks the cut and never splits a code point",
          "[lib][textview][hq-view-escape-fields]") {
  CHECK(tv::truncate_display("abcd", 4) == "abcd");
  CHECK(tv::truncate_display("abcde", 4) == "abc\xE2\x80\xA6");
  CHECK(tv::truncate_display("", 4).empty());
  CHECK(tv::truncate_display("abc", 1) == "\xE2\x80\xA6");
  CHECK(tv::truncate_display("abc", 0) == "\xE2\x80\xA6"); // a width below 1 is 1
  // A wide character that would straddle the limit is left out whole.
  CHECK(tv::truncate_display("ab\xE6\x97\xA5\xE6\x9C\xAC", 4) == "ab\xE2\x80\xA6");
  CHECK(tv::truncate_display("\xE6\x97\xA5\xE6\x9C\xAC", 4) == "\xE6\x97\xA5\xE6\x9C\xAC");
  // Multi-byte text is never cut inside a sequence.
  CHECK(tv::truncate_display("caf\xC3\xA9s", 4) == "caf\xE2\x80\xA6");
  CHECK(tv::display_width(tv::truncate_display(std::string(500, 'x'), tv::k_field_cap)) == tv::k_field_cap);
}

TEST_CASE("textview: cap_field cuts at the field cap", "[lib][textview][hq-view-escape-fields]") {
  CHECK(tv::k_field_cap == 48);
  CHECK(tv::cap_field(std::string(48, 'a')) == std::string(48, 'a'));
  CHECK(tv::cap_field(std::string(49, 'a')) == std::string(47, 'a') + "\xE2\x80\xA6");
}
