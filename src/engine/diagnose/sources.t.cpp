// @file sources.t.cpp
// @brief Source guard for `planar.engine.diagnose` (plan 1132, task 7372).
//
// Task 7333: `planar-watch actions` applied its LIMIT before its filter, so
// a filtered listing silently lost matching rows. The diagnose engine must
// filter in SQL before it limits, and must not borrow that helper. This guard
// reads the engine's own sources: no SQL keyword `limit` and no reference to
// `list_actions` may appear. A later check that really needs a bound adds its
// own filtered subquery and changes this guard deliberately.

#include <catch2/catch_test_macros.hpp>

import std;

namespace {

auto read_source(const char* name) -> std::string {
  std::ifstream in{std::filesystem::path{PLANAR_DIAGNOSE_SOURCE_DIR} / name, std::ios::binary};
  REQUIRE(in.good());
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

auto lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

} // namespace

TEST_CASE("the diagnose engine never limits before filtering", "[diagnose_sources]") {
  for (const char* name : {"diagnose.cpp"}) {
    auto text = lower(read_source(name));
    INFO(name);
    CHECK(text.find("list_actions") == std::string::npos);
    CHECK(text.find(" limit ") == std::string::npos);
    CHECK(text.find(" limit?") == std::string::npos);
    CHECK(text.find("\nlimit ") == std::string::npos);
  }
}

TEST_CASE("the diagnose engine issues only select and pragma reads", "[diagnose_sources]") {
  auto text = lower(read_source("diagnose.cpp"));
  for (const char* verb : {"insert into", "update ", "delete from", "create table", "drop table", "replace into"}) {
    INFO(verb);
    // "update" appears only in no SQL literal; ensure none is quoted.
    CHECK(text.find(std::string{"\""} + verb) == std::string::npos);
  }
}
