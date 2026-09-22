// @file selector.t.cpp
// @brief The engine selector's resolution order and refusals (plan 1033 M0,
// task 6485, decision 1017). Pure: the config read is an injected reader, so
// no sibling `planar` is spawned here — the black-box half lives in
// golden.t.cpp.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;
import planar.cmd.planar_execute.selector;

namespace {

using planar::cmd::execute::config_reader;
using planar::cmd::execute::engine_kind;
using planar::cmd::execute::resolve_engine;
using planar::engine::execute::config_value;

/// @brief A reader that returns `entry` and counts its calls.
/// @param calls Incremented on every call.
/// @param entry What the config plane reports, or unset for "no such key".
/// @return The reader.
auto counting_reader(int& calls, std::optional<config_value> entry) -> config_reader {
  return [&calls, entry = std::move(entry)]() -> std::expected<std::optional<config_value>, std::string> {
    ++calls;
    return entry;
  };
}

} // namespace

TEST_CASE("engine selector: flag beats env beats config beats the default", "[cmd][execute][selector][6485]") {
  config_value const centurion_file{.value = "centurion", .provenance = "config file"};

  SECTION("flag wins, and the config plane is never read") {
    int        calls  = 0;
    auto const choice = resolve_engine("embedded", "centurion", counting_reader(calls, centurion_file));
    REQUIRE(choice.has_value());
    CHECK(choice->engine == engine_kind::embedded);
    CHECK(choice->source == "flag: --engine");
    CHECK(calls == 0);
  }
  SECTION("env wins over config, and the config plane is never read") {
    int        calls  = 0;
    auto const choice = resolve_engine(std::nullopt, "embedded", counting_reader(calls, centurion_file));
    REQUIRE(choice.has_value());
    CHECK(choice->engine == engine_kind::embedded);
    CHECK(choice->source == "env: PLANAR_EXECUTE_ENGINE");
    CHECK(calls == 0);
  }
  SECTION("an EMPTY env is unset, so the config decides") {
    int        calls  = 0;
    auto const choice = resolve_engine(std::nullopt, "", counting_reader(calls, centurion_file));
    REQUIRE(choice.has_value());
    CHECK(choice->engine == engine_kind::centurion);
    CHECK(choice->source == "config file");
    CHECK(calls == 1);
  }
  SECTION("config reports its own provenance verbatim") {
    int        calls = 0;
    auto const choice =
        resolve_engine(std::nullopt, std::nullopt, counting_reader(calls, config_value{"embedded", "embedded default"}));
    REQUIRE(choice.has_value());
    CHECK(choice->engine == engine_kind::embedded);
    CHECK(choice->source == "embedded default");
  }
  SECTION("a sibling planar that does not know the key yields the default") {
    int        calls  = 0;
    auto const choice = resolve_engine(std::nullopt, std::nullopt, counting_reader(calls, std::nullopt));
    REQUIRE(choice.has_value());
    CHECK(choice->engine == engine_kind::embedded);
    CHECK(choice->source == "embedded default");
    CHECK(calls == 1);
  }
}

TEST_CASE("engine selector: a bad env or config value, or an unreadable config, is a named refusal",
          "[cmd][execute][selector][6485]") {
  int calls = 0;

  auto const bad_env = resolve_engine(std::nullopt, "zig", counting_reader(calls, std::nullopt));
  REQUIRE_FALSE(bad_env.has_value());
  CHECK(bad_env.error() == "PLANAR_EXECUTE_ENGINE must be embedded or centurion, got: zig");
  // A bad env is NOT silently skipped in favour of the config.
  CHECK(calls == 0);

  auto const bad_config =
      resolve_engine(std::nullopt, std::nullopt, counting_reader(calls, config_value{"Centurion", "config file"}));
  REQUIRE_FALSE(bad_config.has_value());
  CHECK(bad_config.error() == "execute.engine must be embedded or centurion, got: Centurion (config file)");

  config_reader const failing = []() -> std::expected<std::optional<config_value>, std::string> {
    return std::unexpected{std::string{"planar exited non-zero: boom"}};
  };
  auto const unreadable = resolve_engine(std::nullopt, std::nullopt, failing);
  REQUIRE_FALSE(unreadable.has_value());
  CHECK(unreadable.error() == "cannot read execute.engine from planar config: planar exited non-zero: boom");
}

TEST_CASE("engine selector: profile show renders text and JSON", "[cmd][execute][selector][6485]") {
  planar::cmd::execute::engine_choice const choice{.engine = engine_kind::centurion, .source = "env: PLANAR_EXECUTE_ENGINE"};
  CHECK(planar::cmd::execute::render_profile(choice, false) == "engine: centurion\nengine_source: env: PLANAR_EXECUTE_ENGINE\n");
  CHECK(planar::cmd::execute::render_profile(choice, true) ==
        R"({"engine":"centurion","engine_source":"env: PLANAR_EXECUTE_ENGINE"})"
        "\n");
  // A provenance label that needs escaping is escaped, not spliced.
  planar::cmd::execute::engine_choice const odd{.engine = engine_kind::embedded, .source = "a \"quoted\" source"};
  CHECK(planar::cmd::execute::render_profile(odd, true) == R"({"engine":"embedded","engine_source":"a \"quoted\" source"})"
                                                           "\n");
}
