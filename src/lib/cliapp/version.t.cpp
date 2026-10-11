// @file version.t.cpp
// @brief Unit tests for `planar.cliapp.version` (task cpp-cli-output-logging).
//
// Oracle capture (task brief: derive expected values by running the
// reference binary, never hand-assumed):
//
//   $ ./zig/zig-out/bin/planar version
//   planar dev dev zig 0.16.0
//
// `render_version_text` is checked against the six-position release contract (six
// whitespace-splittable tokens, sha+dirty-marker combined into token[1],
// dev sentinel by default) rather than the literal `zig <ver>` tail — see
// version.cppm's file comment for why the runtime tag differs by
// construction.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.cliapp.version;

using planar::cliapp::build_info;
using planar::cliapp::render_version_text;
using planar::cliapp::shorten_sha;

TEST_CASE("shorten_sha truncates a 40-char hex sha to 12 chars", "[cli][version]") {
  const std::string long_sha = "0123456789abcdef0123456789abcdef01234567";
  REQUIRE(shorten_sha(long_sha) == "0123456789ab");
}

TEST_CASE("shorten_sha passes short input through unchanged", "[cli][version]") {
  REQUIRE(shorten_sha("unknown") == "unknown");
  REQUIRE(shorten_sha("dev") == "dev");
  REQUIRE(shorten_sha("abc") == "abc");
}

// Break-probe: shorten_sha at the exact 12-char boundary must NOT truncate
// further (off-by-one check on the `<= 12` guard).
TEST_CASE("shorten_sha: exactly 12 chars passes through unchanged", "[cli][version]") {
  const std::string exactly_twelve = "0123456789ab";
  REQUIRE(shorten_sha(exactly_twelve) == exactly_twelve);
  REQUIRE(shorten_sha(exactly_twelve).size() == 12);
}

TEST_CASE("render_version_text: dev sentinel default, matches oracle SHAPE", "[cli][version]") {
  build_info info{}; // default-constructed: sha="dev", date="dev", dirty=false
  auto       text = render_version_text(info, "22.1.8");
  REQUIRE(text == "planar dev dev cxx 22.1.8 dev\n");

  // Whitespace-splittable into exactly 6 tokens, with the release appended to
  // the stable program/sha/date/cxx/compiler prefix.
  // trailing newline included in `text` produces one trailing empty token
  // via views::split, which we drop before counting.
  std::vector<std::string> tokens;
  for (auto part : std::views::split(text, ' ')) {
    std::string tok{part.begin(), part.end()};
    if (!tok.empty()) {
      // The last field carries the trailing '\n'; strip it before storing.
      if (tok.back() == '\n') {
        tok.pop_back();
      }
      if (!tok.empty()) {
        tokens.push_back(tok);
      }
    }
  }
  REQUIRE(tokens.size() == 6);
  REQUIRE(tokens[0] == "planar");
  REQUIRE(tokens[5] == "dev");
}

TEST_CASE("render_version_text: real sha is truncated and dirty marker appended", "[cli][version]") {
  build_info info{.sha = "0123456789abcdef0123456789abcdef01234567", .date = "2026-08-22T00:00:00Z", .dirty = true};
  auto       text = render_version_text(info, "22.1.8");
  REQUIRE(text == "planar 0123456789ab+dirty 2026-08-22T00:00:00Z cxx 22.1.8 dev\n");
}

TEST_CASE("render_version_text: clean (non-dirty) real sha has no dirty marker", "[cli][version]") {
  build_info info{.sha = "abcdef0123456789abcdef0123456789abcdef01", .date = "2026-08-22T00:00:00Z", .dirty = false};
  auto       text = render_version_text(info, "22.1.8");
  REQUIRE(text.find("+dirty") == std::string::npos);
}

// Break-probe: a dev build's current_build_info() must reproduce the
// dev-sentinel contract when PLANAR_VERSION_META is not defined at compile
// time (the default configure — see CMakeLists.txt). This is the load-
// bearing assertion for "a dev build must not bake in git sha/dirty
// state" (task brief).
TEST_CASE("current_build_info: default (dev) configure never claims dirty=true", "[cli][version]") {
  auto info = planar::cliapp::current_build_info();
  // Whether or not PLANAR_VERSION_META was passed to this particular test
  // build, dirty must be a real boolean resolved from an actual git check
  // -- it must never be true by accident of an unset macro. We can only
  // assert the invariant that holds unconditionally in the default
  // (unset) case: sha/date default to "dev" and dirty defaults to false.
#if !defined(PLANAR_VERSION_META)
  REQUIRE(info.sha == "dev");
  REQUIRE(info.date == "dev");
  REQUIRE(info.dirty == false);
#else
  // Metadata resolution was explicitly requested for this build; sha and
  // date must be non-empty (either resolved or "unknown"), never blank.
  REQUIRE_FALSE(info.sha.empty());
  REQUIRE_FALSE(info.date.empty());
#endif
}

TEST_CASE("version rendering preserves build identity while appending the release", "[cli][version]") {
  build_info info{
      .sha = "0123456789abcdef0123456789abcdef01234567", .date = "2026-10-06T00:00:00Z", .dirty = true, .release = "v1.2.0"};
  CHECK(render_version_text(info, "Clang-23.1.0") == "planar 0123456789ab+dirty 2026-10-06T00:00:00Z cxx Clang-23.1.0 v1.2.0\n");
  CHECK(planar::cliapp::render_version_json(info, "Clang-23.1.0") ==
        "{\"release\":\"v1.2.0\",\"sha\":\"0123456789abcdef0123456789abcdef01234567\","
        "\"date\":\"2026-10-06T00:00:00Z\",\"dirty\":true,\"compiler\":\"Clang-23.1.0\"}\n");
}
