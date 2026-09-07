// @file follow.t.cpp
// @brief Deterministic unit coverage for
// `planar.cmd.planar_watch.handlers.follow`'s pure/testable pieces
// (duration parsing, the SIGINT flag) plus one deterministic coverage
// case for the Tier-2 wake source's directory-watch fix (task 6450).
//
// The wake source is inherently filesystem-dependent, but the LAST case
// below is not a "sleep and hope" test: the background thread's create
// (and immediate delete) of the `-wal` sibling is scheduled at a fixed
// offset chosen to fall INSIDE the old code's first 100ms blind-wait
// window (see that case's comment for the exact reasoning) — the
// assertion is a generous elapsed-time ceiling that separates "the wake
// fired" from "only the heartbeat fired," not a race whose outcome is
// left to chance. The broader end-to-end cross-process scenario (a real
// `planar-agent pull` against a real `feed --follow` child process) stays
// in the integration suite's four `feed --follow` scenarios (task 6449).
#include <catch2/catch_test_macros.hpp>
#include <csignal>

import std;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handlers.follow;

using planar::cmd::watch::handlers::interval_or_default;
using planar::cmd::watch::handlers::k_default_interval_ns;
using planar::cmd::watch::handlers::parse_duration_ns;

TEST_CASE("follow: parse_duration_ns treats a bare integer as seconds", "[cmd][watch][follow][duration]") {
  CHECK(parse_duration_ns("0") == 0ULL);
  CHECK(parse_duration_ns("1") == 1'000'000'000ULL);
  CHECK(parse_duration_ns("600") == 600ULL * 1'000'000'000ULL);
}

TEST_CASE("follow: parse_duration_ns accepts ns/us/ms/s/m/h suffixes", "[cmd][watch][follow][duration]") {
  CHECK(parse_duration_ns("500ns") == 500ULL);
  CHECK(parse_duration_ns("7us") == 7ULL * 1'000ULL);
  CHECK(parse_duration_ns("500ms") == 500ULL * 1'000'000ULL);
  CHECK(parse_duration_ns("1s") == 1'000'000'000ULL);
  CHECK(parse_duration_ns("10m") == 10ULL * 60ULL * 1'000'000'000ULL);
  CHECK(parse_duration_ns("1h") == 60ULL * 60ULL * 1'000'000'000ULL);
  CHECK(parse_duration_ns("100ms") == 100ULL * 1'000'000ULL);
}

TEST_CASE("follow: parse_duration_ns rejects malformed input", "[cmd][watch][follow][duration]") {
  CHECK_FALSE(parse_duration_ns("").has_value());
  CHECK_FALSE(parse_duration_ns("foo").has_value());
  CHECK_FALSE(parse_duration_ns("5xx").has_value());
  CHECK_FALSE(parse_duration_ns("1xyz").has_value());
  CHECK_FALSE(parse_duration_ns("abc").has_value());
  CHECK_FALSE(parse_duration_ns("ms").has_value());
  CHECK_FALSE(parse_duration_ns("-5s").has_value());
}

TEST_CASE("follow: parse_duration_ns rejects u64-nanosecond overflow rather than saturating", "[cmd][watch][follow][duration]") {
  CHECK_FALSE(parse_duration_ns("18446744073709551615h").has_value());
  CHECK_FALSE(parse_duration_ns("99999999999999999999s").has_value());
}

TEST_CASE("follow: interval_or_default falls back silently on missing or malformed --interval",
          "[cmd][watch][follow][duration]") {
  // Matches the oracle's `ps.parseIntervalOrDefault`: a malformed
  // --interval is NOT a CLI error, it is silently replaced by the
  // default (D2 -- reproduced, not "fixed").
  CHECK(interval_or_default(std::nullopt) == k_default_interval_ns);
  CHECK(interval_or_default(std::optional<std::string>{"not-a-duration"}) == k_default_interval_ns);
  CHECK(interval_or_default(std::optional<std::string>{"30s"}) == 30ULL * 1'000'000'000ULL);
  CHECK(interval_or_default(std::optional<std::string>{"100ms"}) == 100ULL * 1'000'000ULL);
}

TEST_CASE("follow: SIGINT flips should_stop, and reset_for_testing clears it", "[cmd][watch][follow][sigint]") {
  using planar::cmd::watch::handlers::install_sigint_handler;
  using planar::cmd::watch::handlers::reset_for_testing;
  using planar::cmd::watch::handlers::should_stop;

  reset_for_testing();
  CHECK_FALSE(should_stop());

  install_sigint_handler();
  // Deterministic: raise(2) delivers SIGINT to THIS thread synchronously
  // (POSIX: raise() does not return until the handler has run), so there
  // is nothing to sleep-and-poll for here.
  raise(SIGINT);
  CHECK(should_stop());

  reset_for_testing();
  CHECK_FALSE(should_stop());
}

#if defined(__APPLE__) || defined(__linux__)
TEST_CASE("follow: interruptible_sleep wakes on a transient -wal via the parent-directory watch (task 6450)",
          "[cmd][watch][follow][wake]") {
  // ## Why this timing is deterministic, not sleep-and-hope
  //
  // Root cause (task 6450): a one-shot writer (`planar-agent pull`, etc.)
  // creates AND DELETES the `-wal` sibling entirely within its own short
  // process lifetime. The pre-fix code only attempted to attach to that
  // file at fixed 100ms slice boundaries (`k_slice_ns`); a transient file
  // whose entire lifetime falls INSIDE one of those 100ms blind-wait
  // windows is invisible to it no matter how many times the loop retries,
  // because kqueue with zero registered filters cannot wake early.
  //
  // This case reproduces exactly that shape: the background thread
  // creates, and immediately deletes, the `-wal` sibling at a fixed
  // 40ms offset from the moment `interruptible_sleep` starts (i.e.,
  // squarely inside the FIRST [0ms, 100ms) slice, not near either
  // boundary). Against the pre-fix code this is a 100%-reproducible
  // miss — not a probabilistic one — because the transient window's
  // timing is fixed relative to `wake_source`'s construction, which
  // itself happens synchronously at the top of the first
  // `interruptible_sleep` call. Against the fixed code, the parent
  // directory watch is registered at construction (before the loop
  // starts waiting at all), so the mid-window create is queued the
  // instant it happens and `interruptible_sleep` returns almost
  // immediately.
  //
  // The assertion is a generous elapsed-time ceiling (2s) against a 5s
  // configured timeout: "did the wake fire" vs. "only the heartbeat
  // fired," not a tight race against real time. This tolerates ordinary
  // scheduler jitter without becoming a coin flip.
  using planar::cmd::watch::context;
  using planar::cmd::watch::map_env;
  using planar::cmd::watch::handlers::interruptible_sleep;
  using planar::cmd::watch::handlers::reset_for_testing;

  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_watch_follow_wake_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);
  struct cleanup {
    std::filesystem::path p;
    ~cleanup() {
      std::error_code rm_ec;
      std::filesystem::remove_all(p, rm_ec);
    }
  } const guard{root};

  auto const db_path  = root / "planar.db";
  auto const wal_path = root / "planar.db-wal";

  reset_for_testing();

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::vector<std::string>{"planar-watch"},
                         map_env({{"PLANAR_HOME", (root / "home").string()}, {"HOME", (root / "fakehome").string()}}),
                         root,
                         db_path,
                         out,
                         err};

  std::jthread writer([&wal_path] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
      std::ofstream f(wal_path, std::ios::binary);
      f << "x";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::error_code rm_ec;
    std::filesystem::remove(wal_path, rm_ec);
  });

  auto const start = std::chrono::steady_clock::now();
  interruptible_sleep(ctx, 5'000'000'000ULL); // 5s configured heartbeat
  auto const elapsed = std::chrono::steady_clock::now() - start;

  writer.join();
  reset_for_testing();

  CHECK(elapsed < std::chrono::seconds(2));
}
#endif
