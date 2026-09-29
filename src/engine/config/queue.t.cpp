// @file queue.t.cpp
// @brief Unit tests for `planar.engine.config.queue` (plan 1080, task
// hq-config; folded caveat task hq-negative-window). Expectations come from
// tech spec 647 (defaults: one slot, one-second poll, thirty-second staleness
// window, ten-second grace, thirty days of history; slots read at each poll)
// and test-spec scenarios "the queue table validates", "invalid queue
// settings are refused" and "an absent queue table uses the defaults".
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.config.toml;
import planar.engine.config.effective;
import planar.engine.config.queue;

using planar::engine::config::default_queue_settings;
using planar::engine::config::load_queue_settings;
using planar::engine::config::parse_toml;
using planar::engine::config::queue_from_map;
using planar::engine::config::queue_load_error;
using planar::engine::config::queue_settings;
using planar::engine::config::validate_queue;

namespace {

auto doc_of(std::string_view text) -> planar::engine::config::toml_map {
  auto parsed = parse_toml(text);
  REQUIRE(parsed.has_value());
  return *parsed;
}

auto queue_of(std::string_view body) -> std::expected<queue_settings, std::vector<planar::engine::config::queue_finding>> {
  return queue_from_map(doc_of(std::format("[queue]\n{}\n", body)));
}

/// A scratch directory removed at scope exit.
struct scratch {
  std::filesystem::path root;
  scratch() {
    root = std::filesystem::temp_directory_path() /
           std::format("planar_queue_cfg_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
  }
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  scratch(const scratch&)            = delete;
  scratch& operator=(const scratch&) = delete;
  void     write(std::string_view text) const {
    std::ofstream out{root / "config.toml", std::ios::trunc};
    out << text;
  }
  [[nodiscard]] auto path() const -> std::filesystem::path {
    return root / "config.toml";
  }
};

} // namespace

TEST_CASE("queue config: an absent table gives the documented defaults", "[engine][config][hq-config]") {
  auto const got = queue_from_map(doc_of("[defaults]\nvendor = \"claude\"\n"));
  REQUIRE(got.has_value());
  CHECK(got->slots == 1);
  CHECK(got->poll_interval_ms == 1000);
  CHECK(got->stale_after_ms == 30000);
  CHECK(got->grace_ms == 10000);
  CHECK(got->history_days == 30);
  CHECK(*got == default_queue_settings());
}

TEST_CASE("queue config: the shipped defaults.toml agrees with the built-in defaults", "[engine][config][hq-config]") {
  auto const got = queue_from_map(doc_of(planar::engine::config::defaults_toml()));
  REQUIRE(got.has_value());
  CHECK(*got == default_queue_settings());
  CHECK(planar::engine::config::defaults_toml().contains("[queue]"));
}

TEST_CASE("queue config: every key set is accepted and converted to engine units", "[engine][config][hq-config]") {
  auto const got = queue_of("slots = 2\npoll_interval = \"250ms\"\nstale_after = \"5s\"\ngrace = \"3s\"\nhistory_days = 7");
  REQUIRE(got.has_value());
  CHECK(got->slots == 2);
  CHECK(got->poll_interval_ms == 250);
  CHECK(got->stale_after_ms == 5000);
  CHECK(got->grace_ms == 3000);
  CHECK(got->history_days == 7);
}

TEST_CASE("queue config: duration units are milliseconds, seconds, minutes and hours", "[engine][config][hq-config]") {
  auto const got = queue_of("poll_interval = \"2s\"\nstale_after = \"2m\"\ngrace = \"1h\"");
  REQUIRE(got.has_value());
  CHECK(got->poll_interval_ms == 2000);
  CHECK(got->stale_after_ms == 120000);
  CHECK(got->grace_ms == 3600000);
  auto const ms = queue_of("poll_interval = \"1500ms\"\nstale_after = \"1500ms\"\ngrace = \"0s\"");
  REQUIRE(ms.has_value());
  CHECK(ms->poll_interval_ms == 1500);
  CHECK(ms->grace_ms == 0);
}

TEST_CASE("queue config: a partial table keeps defaults for the rest", "[engine][config][hq-config]") {
  auto const got = queue_of("slots = 3");
  REQUIRE(got.has_value());
  CHECK(got->slots == 3);
  CHECK(got->poll_interval_ms == 1000);
  CHECK(got->stale_after_ms == 30000);
  CHECK(got->grace_ms == 10000);
  CHECK(got->history_days == 30);
}

TEST_CASE("queue config: a stale_after shorter than poll_interval is refused, equal is accepted", "[engine][config][hq-config]") {
  auto const bad = queue_of("poll_interval = \"5s\"\nstale_after = \"4s\"");
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().size() == 1);
  CHECK(bad.error()[0].key == "queue.stale_after");
  CHECK(bad.error()[0].message.contains("poll_interval"));
  auto const equal = queue_of("poll_interval = \"5s\"\nstale_after = \"5s\"");
  CHECK(equal.has_value());
  // The default poll interval applies when only stale_after is set.
  auto const short_default = queue_of("stale_after = \"500ms\"");
  REQUIRE_FALSE(short_default.has_value());
  CHECK(short_default.error()[0].key == "queue.stale_after");
}

TEST_CASE("queue config: each invalid value is refused with a message naming its key", "[engine][config][hq-config]") {
  struct bad_case {
    std::string_view body;
    std::string_view key;
  };
  constexpr std::array cases = {
      bad_case{"slots = 0", "queue.slots"},
      bad_case{"slots = -1", "queue.slots"},
      bad_case{"slots = \"two\"", "queue.slots"},
      bad_case{"slots = true", "queue.slots"},
      bad_case{"slots = 100000", "queue.slots"},
      bad_case{"history_days = 0", "queue.history_days"},
      bad_case{"history_days = -3", "queue.history_days"},
      bad_case{"history_days = \"30\"", "queue.history_days"},
      bad_case{"poll_interval = \"0s\"", "queue.poll_interval"},
      bad_case{"poll_interval = \"-1s\"", "queue.poll_interval"},
      bad_case{"poll_interval = 1", "queue.poll_interval"},
      bad_case{"poll_interval = \"1\"", "queue.poll_interval"},
      bad_case{"poll_interval = \"fast\"", "queue.poll_interval"},
      bad_case{"poll_interval = \"1.5s\"", "queue.poll_interval"},
      bad_case{"poll_interval = \"1d\"", "queue.poll_interval"},
      bad_case{"poll_interval = \"\"", "queue.poll_interval"},
      bad_case{"stale_after = \"-5s\"", "queue.stale_after"},
      bad_case{"stale_after = \"-1ms\"", "queue.stale_after"},
      bad_case{"stale_after = \"0s\"", "queue.stale_after"},
      bad_case{"stale_after = \"0ms\"", "queue.stale_after"},
      bad_case{"stale_after = -30", "queue.stale_after"},
      bad_case{"grace = \"-1s\"", "queue.grace"},
      bad_case{"grace = 10", "queue.grace"},
      bad_case{"grace = \"999999h\"", "queue.grace"},
      bad_case{"grace = \"9223372036854775807h\"", "queue.grace"},
      bad_case{"unknown_key = 1", "queue.unknown_key"},
      bad_case{"slot = 2", "queue.slot"},
  };
  for (auto const& one : cases) {
    INFO("body: " << one.body);
    auto const got = queue_of(one.body);
    REQUIRE_FALSE(got.has_value());
    REQUIRE_FALSE(got.error().empty());
    CHECK(got.error()[0].key == one.key);
    CHECK(got.error()[0].message.contains(std::string{one.key.substr(6)}));
  }
}

TEST_CASE("queue config: every finding is reported, not just the first", "[engine][config][hq-config]") {
  auto const findings = validate_queue(doc_of("[queue]\nslots = 0\nstale_after = \"-1s\"\nhistory_days = 0\n"));
  REQUIRE(findings.size() == 3);
  std::vector<std::string> keys;
  for (auto const& one : findings) {
    keys.push_back(one.key);
  }
  CHECK(std::ranges::contains(keys, "queue.slots"));
  CHECK(std::ranges::contains(keys, "queue.stale_after"));
  CHECK(std::ranges::contains(keys, "queue.history_days"));
}

TEST_CASE("queue config: settings that pass never carry a non-positive window, interval or slot count",
          "[engine][config][hq-config][hq-negative-window]") {
  // Property over a grid: whatever combination of values is offered, an
  // accepted result satisfies the ranges the engine relies on, and a value
  // outside them is never accepted.
  constexpr std::array<std::string_view, 8> durations = {"-1s", "0s", "1ms", "250ms", "1s", "30s", "-5m", "2h"};
  constexpr std::array<std::string_view, 4> slots     = {"-1", "0", "1", "4"};
  std::size_t                               accepted  = 0;
  for (auto const stale : durations) {
    for (auto const poll : durations) {
      for (auto const slot : slots) {
        auto const got = queue_of(std::format("slots = {}\npoll_interval = \"{}\"\nstale_after = \"{}\"", slot, poll, stale));
        if (!got.has_value()) {
          continue;
        }
        ++accepted;
        CHECK(got->slots >= 1);
        CHECK(got->poll_interval_ms > 0);
        CHECK(got->stale_after_ms > 0);
        CHECK(got->stale_after_ms >= got->poll_interval_ms);
      }
    }
  }
  CHECK(accepted > 10); // the grid does contain valid combinations
}

TEST_CASE("queue loader: a missing file gives the defaults", "[engine][config][hq-config]") {
  scratch const dir;
  auto const    got = load_queue_settings(dir.path());
  REQUIRE(got.has_value());
  CHECK(*got == default_queue_settings());
}

TEST_CASE("queue loader: the file's values are returned, and an edit is seen on the next call", "[engine][config][hq-config]") {
  scratch const dir;
  dir.write("[queue]\nslots = 2\nstale_after = \"7s\"\n");
  auto const first = load_queue_settings(dir.path());
  REQUIRE(first.has_value());
  CHECK(first->slots == 2);
  CHECK(first->stale_after_ms == 7000);
  // Lowering slots takes effect at the next poll: the loader keeps no cache.
  dir.write("[queue]\nslots = 1\nstale_after = \"9s\"\n");
  auto const second = load_queue_settings(dir.path());
  REQUIRE(second.has_value());
  CHECK(second->slots == 1);
  CHECK(second->stale_after_ms == 9000);
}

TEST_CASE("queue loader: a file with no queue table gives the defaults", "[engine][config][hq-config]") {
  scratch const dir;
  dir.write("[defaults]\nvendor = \"codex\"\n");
  auto const got = load_queue_settings(dir.path());
  REQUIRE(got.has_value());
  CHECK(*got == default_queue_settings());
}

TEST_CASE("queue loader: an invalid table is an error carrying the findings, never settings", "[engine][config][hq-config]") {
  scratch const dir;
  dir.write("[queue]\nstale_after = \"-5s\"\n");
  auto const got = load_queue_settings(dir.path());
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind_ == queue_load_error::kind::invalid);
  REQUIRE(got.error().findings.size() == 1);
  CHECK(got.error().findings[0].key == "queue.stale_after");
}

TEST_CASE("queue loader: malformed TOML is a parse failure", "[engine][config][hq-config]") {
  scratch const dir;
  dir.write("[queue\nslots = 2\n");
  auto const got = load_queue_settings(dir.path());
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind_ == queue_load_error::kind::parse_failed);
}

TEST_CASE("queue loader: an unreadable path is reported, not defaulted", "[engine][config][hq-config]") {
  scratch const dir;
  // A directory where the file should be: exists, cannot be read as a file.
  std::filesystem::create_directories(dir.path());
  auto const got = load_queue_settings(dir.path());
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind_ == queue_load_error::kind::unreadable);
}
