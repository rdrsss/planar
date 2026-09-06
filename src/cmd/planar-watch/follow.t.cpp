/// @file follow.t.cpp
/// @brief Deterministic unit coverage for
/// `planar.cmd.planar_watch.handlers.follow`'s pure/testable pieces:
/// duration parsing and the SIGINT flag. The Tier-2 wake source itself
/// (kqueue/inotify against a real `-wal` file) is inherently
/// filesystem-and-timing dependent and is instead covered by the
/// integration suite's four `feed --follow` scenarios (task 6449) — this
/// file deliberately does NOT sleep-and-hope over real fds, per this
/// task's "test discipline" directive.
#include <catch2/catch_test_macros.hpp>
#include <csignal>

import std;
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

TEST_CASE("follow: parse_duration_ns rejects u64-nanosecond overflow rather than saturating",
          "[cmd][watch][follow][duration]") {
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
