// @file settings.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.settings`. Covers the
// settings scenarios "Settings read from the table with defaults for missing
// keys" and "Out-of-range and unknown settings are refused", the duration
// grammar the settings share with the command-line flags, and the loader's
// refusal of a hand-edited row.
//
// Every case that reads settings opens its own build queue database in its
// own temp directory, migrated with the embedded build queue chain. The
// scenarios write rows with raw SQL, because the `config set` verb that
// writes them is a later task.
//
// Include-before-import is deliberate (see db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.hostqueue;

namespace {

namespace hq = planar::engine::hostqueue;

/// @brief A unique scratch directory under the system temp directory,
/// removed (recursively, best-effort) when the guard goes out of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_settings_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
};

/// @brief A build queue database at the head of the embedded build queue chain.
auto open_build_queue(const scratch_dir& scratch) -> planar::db::connection {
  auto opened = planar::db::connection::open((scratch.path_ / "build-queue.db").string());
  REQUIRE(opened.has_value());
  auto applied = planar::db::apply_build_queue(*opened);
  REQUIRE(applied.has_value());
  return std::move(*opened);
}

/// @brief Writes one row of `queue_settings`, replacing any row with the same key.
void store(planar::db::connection& conn, std::string_view key, std::string_view value) {
  auto stmt = conn.prepare("insert or replace into queue_settings (key, value) values (?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, key).has_value());
  REQUIRE(stmt->bind_text(2, value).has_value());
  REQUIRE(stmt->step().has_value());
}

} // namespace

TEST_CASE("settings: an empty table reads as the documented defaults", "[hostqueue][settings]") {
  scratch_dir const scratch;
  auto              conn = open_build_queue(scratch);
  auto const        got  = hq::load_settings(conn);
  REQUIRE(got.has_value());
  CHECK(got->slots == 1);
  CHECK(got->poll_interval_ms == 1000);
  CHECK(got->stale_after_ms == 30000);
  CHECK(got->grace_ms == 10000);
  CHECK(got->history_days == 30);
  CHECK(got->tagged_history_days == 90);
  CHECK(*got == hq::default_queue_settings());
}

TEST_CASE("settings: a stored row is read at the next load, without reopening the connection", "[hostqueue][settings]") {
  scratch_dir const scratch;
  auto              conn = open_build_queue(scratch);
  store(conn, "slots", "3");
  auto const first = hq::load_settings(conn);
  REQUIRE(first.has_value());
  CHECK(first->slots == 3);
  CHECK(first->poll_interval_ms == 1000); // a key not stored keeps its default
  // Changed on the same connection: the next poll sees the new value.
  store(conn, "slots", "1");
  auto const second = hq::load_settings(conn);
  REQUIRE(second.has_value());
  CHECK(second->slots == 1);
}

TEST_CASE("settings: every key stored is read and converted to engine units", "[hostqueue][settings]") {
  scratch_dir const scratch;
  auto              conn = open_build_queue(scratch);
  store(conn, "slots", "2");
  store(conn, "poll_interval", "250ms");
  store(conn, "stale_after", "5s");
  store(conn, "grace", "3s");
  store(conn, "history_days", "7");
  store(conn, "tagged_history_days", "14");
  auto const got = hq::load_settings(conn);
  REQUIRE(got.has_value());
  CHECK(got->slots == 2);
  CHECK(got->poll_interval_ms == 250);
  CHECK(got->stale_after_ms == 5000);
  CHECK(got->grace_ms == 3000);
  CHECK(got->history_days == 7);
  CHECK(got->tagged_history_days == 14);
}

TEST_CASE("settings: a stored value that fails validation is refused by the load, naming its key", "[hostqueue][settings]") {
  scratch_dir const scratch;
  auto              conn = open_build_queue(scratch);
  store(conn, "slots", "0");
  auto const got = hq::load_settings(conn);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind_ == hq::settings_error::kind::refused);
  CHECK(got.error().key == "slots");
  CHECK(got.error().message.contains("slots"));
}

TEST_CASE("settings: a stored window that is shorter than the poll interval is refused by the load", "[hostqueue][settings]") {
  scratch_dir const scratch;
  auto              conn = open_build_queue(scratch);
  store(conn, "poll_interval", "10s");
  store(conn, "stale_after", "5s");
  auto const got = hq::load_settings(conn);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind_ == hq::settings_error::kind::refused);
  CHECK(got.error().key == "stale_after");
  CHECK(got.error().message.contains("poll_interval"));
}

TEST_CASE("settings: a missing queue_settings table is a database error, never the defaults", "[hostqueue][settings]") {
  scratch_dir const scratch;
  auto              opened = planar::db::connection::open((scratch.path_ / "empty.db").string());
  REQUIRE(opened.has_value());
  auto const got = hq::load_settings(*opened);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().kind_ == hq::settings_error::kind::database);
  CHECK(got.error().key.empty());
  CHECK(got.error().message.contains("queue_settings"));
}

TEST_CASE("settings: each out-of-range, malformed or unknown setting is refused with a message naming its key",
          "[hostqueue][settings]") {
  struct refusal_case {
    std::string_view         key;
    std::string_view         value;
    hq::settings_error::kind kind_;
  };
  using enum hq::settings_error::kind;
  constexpr std::array cases = {
      refusal_case{"slots", "0", refused},
      refusal_case{"slots", "1025", refused},
      refusal_case{"slots", "-1", refused},
      refusal_case{"slots", "two", refused},
      refusal_case{"slots", "2.0", refused},
      refusal_case{"slots", "+2", refused},
      refusal_case{"history_days", "0", refused},
      refusal_case{"history_days", "36501", refused},
      refusal_case{"tagged_history_days", "0", refused},
      refusal_case{"tagged_history_days", "36501", refused},
      refusal_case{"poll_interval", "0s", refused},
      refusal_case{"poll_interval", "-1s", refused},
      refusal_case{"poll_interval", "1", refused}, // a bare integer is refused
      refusal_case{"poll_interval", "1.5s", refused},
      refusal_case{"poll_interval", "1d", refused}, // days belong to the history flag only
      refusal_case{"poll_interval", "", refused},
      refusal_case{"poll_interval", "fast", refused},
      refusal_case{"stale_after", "-5s", refused},
      refusal_case{"stale_after", "0ms", refused},
      refusal_case{"stale_after", "30", refused}, // a bare-integer duration
      refusal_case{"grace", "-1s", refused},
      refusal_case{"grace", "10", refused}, // a bare-integer duration
      refusal_case{"grace", "999999h", refused},
      refusal_case{"bogus", "1", unknown_key},
      refusal_case{"slot", "2", unknown_key},
  };
  for (auto const& one : cases) {
    INFO("setting: " << one.key << " = " << one.value);
    auto const got = hq::validate_setting(one.key, one.value, hq::default_queue_settings());
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().kind_ == one.kind_);
    CHECK(got.error().key == one.key);
    CHECK(got.error().message.contains(std::string{one.key}));
  }
}

TEST_CASE("settings: stale_after below poll_interval is refused in either direction, equal is accepted",
          "[hostqueue][settings]") {
  auto const five_second_poll = hq::queue_settings{.poll_interval_ms = 5000};
  auto const short_window     = hq::validate_setting("stale_after", "4s", five_second_poll);
  REQUIRE_FALSE(short_window.has_value());
  CHECK(short_window.error().key == "stale_after");
  CHECK(short_window.error().message.contains("poll_interval"));
  // The change that breaks the window is the one refused: poll_interval 60s
  // against the default 30s stale_after.
  auto const long_poll = hq::validate_setting("poll_interval", "60s", hq::default_queue_settings());
  REQUIRE_FALSE(long_poll.has_value());
  CHECK(long_poll.error().key == "poll_interval");
  CHECK(long_poll.error().message.contains("stale_after"));
  CHECK(hq::validate_setting("stale_after", "5s", five_second_poll).has_value());
}

TEST_CASE("settings: a valid pair returns the candidate settings with that value applied and nothing else changed",
          "[hostqueue][settings]") {
  auto const base  = hq::default_queue_settings();
  auto const slots = hq::validate_setting("slots", "4", base);
  REQUIRE(slots.has_value());
  CHECK(slots->slots == 4);
  CHECK(slots->poll_interval_ms == base.poll_interval_ms);
  auto const tagged = hq::validate_setting("tagged_history_days", "14", base);
  REQUIRE(tagged.has_value());
  CHECK(tagged->tagged_history_days == 14);
  CHECK(tagged->history_days == base.history_days);
  auto const grace = hq::validate_setting("grace", "0s", base);
  REQUIRE(grace.has_value());
  CHECK(grace->grace_ms == 0);
  auto const stale = hq::validate_setting("stale_after", "250ms", hq::queue_settings{.poll_interval_ms = 250});
  REQUIRE(stale.has_value());
  CHECK(stale->stale_after_ms == 250);
}

TEST_CASE("settings: a grid of poll and stale values never accepts a non-positive window or interval", "[hostqueue][settings]") {
  constexpr std::array<std::string_view, 8> durations = {"-1s", "0s", "1ms", "250ms", "1s", "30s", "-5m", "2h"};
  constexpr std::array<std::string_view, 4> slots     = {"-1", "0", "1", "4"};
  std::size_t                               accepted  = 0;
  for (auto const stale : durations) {
    for (auto const poll : durations) {
      for (auto const slot : slots) {
        auto const with_poll = hq::validate_setting("poll_interval", poll, hq::default_queue_settings());
        if (!with_poll.has_value()) {
          continue;
        }
        auto const with_stale = hq::validate_setting("stale_after", stale, *with_poll);
        if (!with_stale.has_value()) {
          continue;
        }
        auto const got = hq::validate_setting("slots", slot, *with_stale);
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
  CHECK(accepted > 0); // the grid does contain valid combinations
}

TEST_CASE("settings: a duration flag takes the settings grammar and refuses zero, negatives and the unitless",
          "[hostqueue][settings][flags]") {
  CHECK(hq::parse_duration("250ms") == std::int64_t{250});
  CHECK(hq::parse_duration("30s") == std::int64_t{30'000});
  CHECK(hq::parse_duration("5m") == std::int64_t{300'000});
  CHECK(hq::parse_duration("1h") == std::int64_t{3'600'000});
  // The one-day cap is shared, and inclusive.
  CHECK(hq::parse_duration("24h") == std::int64_t{86'400'000});
  CHECK_FALSE(hq::parse_duration("25h").has_value());
  CHECK_FALSE(hq::parse_duration("86400001ms").has_value());
  CHECK_FALSE(hq::parse_duration("99999999999999999999h").has_value());
  // A flag has no meaningful zero.
  CHECK_FALSE(hq::parse_duration("0s").has_value());
  CHECK_FALSE(hq::parse_duration("0ms").has_value());
  CHECK_FALSE(hq::parse_duration("-5s").has_value());
  // A bare integer is refused rather than guessed at, and so is anything else.
  CHECK_FALSE(hq::parse_duration("5").has_value());
  CHECK_FALSE(hq::parse_duration("").has_value());
  CHECK_FALSE(hq::parse_duration("s").has_value());
  CHECK_FALSE(hq::parse_duration("5d").has_value());
  CHECK_FALSE(hq::parse_duration("1d").has_value()); // days belong to the history flag only
  CHECK_FALSE(hq::parse_duration("1.5s").has_value());
  CHECK_FALSE(hq::parse_duration(" 5s").has_value());
  // The refusal names the value.
  CHECK(hq::parse_duration("7x").error().contains("7x"));
}

TEST_CASE("settings: a history --since takes days and the retention maximum as its cap", "[hostqueue][settings][flags]") {
  CHECK(hq::parse_history_since("250ms") == std::int64_t{250});
  CHECK(hq::parse_history_since("30s") == std::int64_t{30'000});
  CHECK(hq::parse_history_since("5m") == std::int64_t{300'000});
  CHECK(hq::parse_history_since("48h") == std::int64_t{172'800'000}); // beyond the one-day flag cap
  CHECK(hq::parse_history_since("7d") == std::int64_t{604'800'000});
  CHECK(hq::parse_history_since("36500d") == std::int64_t{36500} * 86'400'000);
  CHECK(hq::parse_history_since("876000h") == std::int64_t{36500} * 86'400'000);
  CHECK_FALSE(hq::parse_history_since("36501d").has_value());
  CHECK_FALSE(hq::parse_history_since("876001h").has_value());
  CHECK_FALSE(hq::parse_history_since("99999999999999999999d").has_value());
  CHECK_FALSE(hq::parse_history_since("0d").has_value());
  CHECK_FALSE(hq::parse_history_since("-1d").has_value());
  CHECK_FALSE(hq::parse_history_since("7").has_value());
  CHECK_FALSE(hq::parse_history_since("d").has_value());
  CHECK_FALSE(hq::parse_history_since("1w").has_value());
  CHECK_FALSE(hq::parse_history_since("1.5d").has_value());
  CHECK(hq::parse_history_since("36501d").error().contains("36501d"));
  // The shared flag grammar is untouched: no days, one-day cap.
  CHECK_FALSE(hq::parse_duration("1d").has_value());
  CHECK_FALSE(hq::parse_duration("48h").has_value());
}
