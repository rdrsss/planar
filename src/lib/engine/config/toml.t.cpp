// @file toml.t.cpp
// @brief Unit tests for `planar.engine.config.toml` (plan 996, task 6032).
// Exercises the Glaze-backed TOML-to-dotted-map flattener against the same
// shapes zig/src/engine/config/parse.zig's own test suite pins (section
// headers, dotted/quoted table headers, string arrays, ints, bools) plus
// the unsupported-shape rejections (float values, non-string array
// elements) this module's restricted schema does not accept.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.config.toml;

using planar::engine::config::parse_toml;
using planar::engine::config::toml_error;
using planar::engine::config::toml_value;

TEST_CASE("parse_toml: empty document returns an empty map", "[toml]") {
  auto result = parse_toml("");
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("parse_toml: comments-only document returns an empty map", "[toml]") {
  auto result = parse_toml("# a comment\n# another\n");
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("parse_toml: top-level string key", "[toml]") {
  auto result = parse_toml("vendor = \"claude\"\n");
  REQUIRE(result.has_value());
  auto it = result->find("vendor");
  REQUIRE(it != result->end());
  CHECK(it->second.kind_ == toml_value::kind::string);
  CHECK(it->second.string_ == "claude");
}

TEST_CASE("parse_toml: section header creates dotted keys", "[toml]") {
  auto result = parse_toml("[defaults]\nvendor = \"claude\"\nscope = \"global\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("defaults.vendor").string_ == "claude");
  CHECK(result->at("defaults.scope").string_ == "global");
}

TEST_CASE("parse_toml: dotted table header [external.jira.status]", "[toml]") {
  auto result = parse_toml("[external.jira.status]\ntodo = \"To Do\"\ndoing = \"In Progress\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("external.jira.status.todo").string_ == "To Do");
  CHECK(result->at("external.jira.status.doing").string_ == "In Progress");
}

TEST_CASE("parse_toml: quoted table header key flattens to the same bare dotted key", "[toml]") {
  auto result = parse_toml("[external.\"github-issues\"]\nauth = \"gh-cli\"\n");
  REQUIRE(result.has_value());
  CHECK(result->at("external.github-issues.auth").string_ == "gh-cli");
}

TEST_CASE("parse_toml: bool true and false", "[toml]") {
  auto result = parse_toml("enabled = true\ndisabled = false\n");
  REQUIRE(result.has_value());
  CHECK(result->at("enabled").kind_ == toml_value::kind::boolean);
  CHECK(result->at("enabled").bool_ == true);
  CHECK(result->at("disabled").bool_ == false);
}

TEST_CASE("parse_toml: integer value", "[toml]") {
  auto result = parse_toml("count = 42\n");
  REQUIRE(result.has_value());
  CHECK(result->at("count").kind_ == toml_value::kind::integer);
  CHECK(result->at("count").int_ == 42);
}

TEST_CASE("parse_toml: string array", "[toml]") {
  auto result = parse_toml("parent_field_names = [\"Parent\", \"Initiative\", \"Tracking\"]\n");
  REQUIRE(result.has_value());
  const auto& v = result->at("parent_field_names");
  REQUIRE(v.kind_ == toml_value::kind::array);
  REQUIRE(v.array_.size() == 3);
  CHECK(v.array_[0] == "Parent");
  CHECK(v.array_[1] == "Initiative");
  CHECK(v.array_[2] == "Tracking");
}

TEST_CASE("parse_toml: a non-string array element is rejected", "[toml]") {
  auto result = parse_toml("mixed = [\"a\", 1]\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == toml_error::parse_failed);
}

TEST_CASE("parse_toml: a float value is rejected", "[toml]") {
  auto result = parse_toml("ratio = 0.5\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == toml_error::parse_failed);
}

TEST_CASE("parse_toml: malformed document is rejected", "[toml]") {
  auto result = parse_toml("vendor = \"unclosed\n");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == toml_error::parse_failed);
}
