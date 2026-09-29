// @file queue_config.t.cpp
// @brief Black-box tests for the `[queue]` table through the built `planar`
// binary (plan 1080, task hq-config; test spec scenarios "the queue table
// validates" and "invalid queue settings are refused"). Each case writes a
// config.toml into its own arena, runs `planar config validate` under
// `run_pinned`, and asserts the exit code and the message bytes.
#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace {

using ::planar::cmd::parity::make_arena;
using ::planar::cmd::parity::run_pinned;

auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// Writes `config_text` as the arena's config.toml and runs `planar config validate`.
auto validate_with(std::string_view tag, std::string_view config_text) -> ::planar::cmd::parity::capture {
  auto const space = make_arena(tag);
  {
    std::ofstream out{space.cpp_root / "config.toml", std::ios::trunc};
    out << config_text;
  }
  return run_pinned(cpp_bin(), std::vector<std::string>{"config", "validate"}, space.cpp_root, std::string{tag});
}

} // namespace

TEST_CASE("queue config workflow: a full queue table validates and an absent one does too", "[cmd][planar][hq-config]") {
  auto const full = validate_with("qcfg_ok", "[queue]\nslots = 2\npoll_interval = \"500ms\"\nstale_after = \"5s\"\ngrace = \"3s\"\nhistory_days = 7\n");
  INFO("stderr: " << full.err);
  CHECK(full.code == 0);
  CHECK(full.out == "config validate: ok\n");

  auto const absent = validate_with("qcfg_absent", "[defaults]\nvendor = \"claude\"\n");
  CHECK(absent.code == 0);
  CHECK(absent.out == "config validate: ok\n");
}

TEST_CASE("queue config workflow: invalid queue settings are refused at exit 1 naming the key", "[cmd][planar][hq-config]") {
  struct bad_case {
    std::string_view body;
    std::string_view key;
  };
  constexpr std::array cases = {
      bad_case{"slots = 0", "queue.slots"},
      bad_case{"slots = -1", "queue.slots"},
      bad_case{"slots = \"two\"", "queue.slots"},
      bad_case{"stale_after = \"-5s\"", "queue.stale_after"},
      bad_case{"stale_after = \"0s\"", "queue.stale_after"},
      bad_case{"poll_interval = \"10s\"\nstale_after = \"5s\"", "queue.stale_after"},
      bad_case{"history_days = 0", "queue.history_days"},
      bad_case{"grace = \"-1s\"", "queue.grace"},
  };
  std::size_t index = 0;
  for (auto const& one : cases) {
    INFO("body: " << one.body);
    auto const ran = validate_with(std::format("qcfg_bad{}", index++), std::format("[queue]\n{}\n", one.body));
    CHECK(ran.code == 1);
    CHECK(ran.out.empty());
    CHECK(ran.err.contains(std::format("error: {}:", one.key)));
  }
}

TEST_CASE("queue config workflow: a float slot count is refused naming the key", "[cmd][planar][hq-config]") {
  auto const ran = validate_with("qcfg_float", "[queue]\nslots = 1.5\n");
  CHECK(ran.code == 1);
  CHECK(ran.err.contains("slots"));
}

TEST_CASE("queue config workflow: findings from the queue table and another table are all reported",
          "[cmd][planar][hq-config]") {
  auto const ran = validate_with("qcfg_multi", "[external.github-issues]\nauth = \"bogus\"\n[queue]\nslots = 0\n");
  CHECK(ran.code == 1);
  CHECK(ran.err.contains("external.github-issues.auth"));
  CHECK(ran.err.contains("queue.slots"));
}
