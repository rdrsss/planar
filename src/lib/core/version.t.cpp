// @file version.t.cpp
// @brief Unit test for `planar.core` — closes cycle 3's documented seed-test
// gap (cmake/module.cmake: this module previously had no *.t.cpp files
// because Catch2 was not vendored yet; task 6023 vendors it and adds this
// file in the same cycle). Exercises the module's real export surface: a
// version string with the shape `MAJOR.MINOR.PATCH`, matching the project()
// VERSION declared in the top-level CMakeLists.txt.
//
// Include-before-import is deliberate, mirroring tabula's core seed test:
// MSVC's supported direction for mixing textual std headers with IFC
// imports is include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.core;

TEST_CASE("core module reports a non-empty version string", "[core][version]") {
  const char* v = version();
  REQUIRE(v != nullptr);
  std::string_view sv{v};
  REQUIRE_FALSE(sv.empty());
}

TEST_CASE("core module version string has MAJOR.MINOR.PATCH shape", "[core][version]") {
  std::string_view sv{version()};
  auto first_dot = sv.find('.');
  REQUIRE(first_dot != std::string_view::npos);
  auto second_dot = sv.find('.', first_dot + 1);
  REQUIRE(second_dot != std::string_view::npos);

  auto major = sv.substr(0, first_dot);
  auto minor = sv.substr(first_dot + 1, second_dot - first_dot - 1);
  auto patch = sv.substr(second_dot + 1);

  auto is_digits = [](std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
  };
  REQUIRE(is_digits(major));
  REQUIRE(is_digits(minor));
  REQUIRE(is_digits(patch));
}

TEST_CASE("core module version string matches the project() VERSION pin", "[core][version]") {
  REQUIRE(std::string_view{version()} == "0.1.0");
}
