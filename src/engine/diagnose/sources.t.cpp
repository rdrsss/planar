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

auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream in{path, std::ios::binary};
  REQUIRE(in.good());
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Every non-test `*.cpp` and `*.cppm` under the engine's directory, so a later check's own file is covered.
auto engine_sources() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator{PLANAR_DIAGNOSE_SOURCE_DIR}) {
    auto name = entry.path().filename().string();
    if ((name.ends_with(".cpp") || name.ends_with(".cppm")) && !name.ends_with(".t.cpp")) {
      out.push_back(entry.path());
    }
  }
  std::ranges::sort(out);
  return out;
}

auto lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

} // namespace

TEST_CASE("the diagnose engine never limits before filtering", "[diagnose_sources]") {
  auto sources = engine_sources();
  // diagnose.cpp, diagnose.cppm and the five family files; a later check adds its own file here.
  REQUIRE(sources.size() >= 7);
  for (const auto& path : sources) {
    auto text = lower(read_file(path));
    INFO(path.filename().string());
    // The ban on a bare `limit` is deliberately blunt; a check that really needs a bound adds a
    // filtered subquery and changes this guard on purpose.
    CHECK(text.find(" limit ") == std::string::npos);
    CHECK(text.find(" limit?") == std::string::npos);
    CHECK(text.find("\nlimit ") == std::string::npos);
    if (!path.string().ends_with(".cppm")) {
      CHECK(text.find("list_actions") == std::string::npos);
    }
  }
}

TEST_CASE("the diagnose engine issues only select and pragma reads", "[diagnose_sources]") {
  for (const auto& path : engine_sources()) {
    auto text = lower(read_file(path));
    INFO(path.filename().string());
    for (const char* verb : {"insert into", "update ", "delete from", "create table", "drop table", "replace into"}) {
      INFO(verb);
      CHECK(text.find(std::string{"\""} + verb) == std::string::npos);
    }
  }
}
