// @file config.t.cpp
// @brief `find_config_value` over `planar config show --json` output (plan
// 1033 M0, task 6485).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;

using planar::engine::execute::find_config_value;

TEST_CASE("find_config_value picks one key out of planar's line-per-key JSON", "[engine][execute][config][6485]") {
  // The shape `planar config show --json` prints: one object per line.
  constexpr std::string_view k_ndjson = R"({"key":"defaults.scope","value":"global","provenance":"embedded default"})"
                                        "\n"
                                        R"({"key":"execute.engine","value":"centurion","provenance":"config file"})"
                                        "\n"
                                        R"({"key":"workbench.root","value":"/w","provenance":"env: PLANAR_WORKBENCH_ROOT"})"
                                        "\n";

  auto const hit = find_config_value(k_ndjson, "execute.engine");
  REQUIRE(hit.has_value());
  CHECK(hit->value == "centurion");
  CHECK(hit->provenance == "config file");

  // A key that is a PREFIX of another is not a match for it.
  CHECK_FALSE(find_config_value(k_ndjson, "execute").has_value());
  CHECK_FALSE(find_config_value(k_ndjson, "execute.engine.x").has_value());
  CHECK_FALSE(find_config_value("", "execute.engine").has_value());
}

TEST_CASE("find_config_value tolerates a newer planar and skips lines it cannot decode", "[engine][execute][config][6485]") {
  constexpr std::string_view k_ndjson = "not json at all\n"
                                        "\n"
                                        R"({"key":"execute.engine","value":"a \"q\"","provenance":"config file","extra":1})";
  auto const                 hit      = find_config_value(k_ndjson, "execute.engine");
  REQUIRE(hit.has_value());
  // Escapes decoded by the JSON reader, not spliced verbatim.
  CHECK(hit->value == "a \"q\"");
  CHECK(hit->provenance == "config file");
}
