/// @file parity_strict.hpp
/// @brief The strictness switch for oracle-gated parity cases (plan 996,
/// task 6071).
///
/// ## The problem this closes
///
/// Every differential case in this tree is gated on the Zig oracle being
/// built, and answers a missing oracle with Catch2's `SKIP`. `ctest` is
/// configured with `SKIP_RETURN_CODE 4` (cmake/module.cmake), so a skipped
/// case is not a failure and the run still exits 0. That is correct for a
/// developer who has not built `zig/`. It is NOT correct anywhere the
/// parity lane is the thing being relied on, because the headline reads
/// "N tests passed" whether the oracle answered or was never consulted.
///
/// Two things make that worse than an ordinary silent skip:
///
///   - The count is large and easy to under-estimate. The task filed
///     against this named seven sites by file:line, in files
///     (`src/lib/cli/{help,parser,schema}.t.cpp`) that no longer exist
///     after the cliapp split. The real figure at the time of the fix is
///     TWENTY-SIX oracle-gated sites across the four `src/cmd/*/parity.t.cpp`
///     files.
///   - At the M10 cutover, `zig/` is DELETED. Every one of those cases then
///     skips permanently, silently, and forever — a whole differential
///     suite that still reports as passing while asserting nothing.
///
/// ## The switch
///
/// Setting `PLANAR_PARITY_STRICT` to anything other than `0` or the empty
/// string turns a missing oracle from a SKIP into a FAILURE. Unset (the
/// default), behaviour is exactly as before, so a developer without `zig/`
/// built is not blocked.
///
/// Use `PLANAR_REQUIRE_ORACLE` rather than a bare `SKIP` at every site that
/// needs the oracle; `scripts/ctest-report.sh` surfaces the skip tally so a
/// non-strict run still SAYS how much of the lane ran.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <string_view>

namespace planar::parity {

/// @brief Whether a missing oracle should FAIL rather than SKIP.
///
/// Read fresh on every call rather than cached: `catch_discover_tests` runs
/// each case as its own process, so there is no cost, and a cached value
/// would make the switch depend on which case happened to read it first.
/// @return `true` when `PLANAR_PARITY_STRICT` is set to something truthy.
[[nodiscard]] inline auto strict_mode() -> bool {
  char const* const raw = std::getenv("PLANAR_PARITY_STRICT");
  if (raw == nullptr) {
    return false;
  }
  std::string_view const value{raw};
  return !value.empty() && value != "0";
}

} // namespace planar::parity

/// @brief Skip — or, under `PLANAR_PARITY_STRICT`, FAIL — when the oracle
/// is unavailable.
///
/// A statement macro rather than a function because Catch2's `SKIP` and
/// `FAIL` both have to expand inside the test body to register against the
/// running case.
#define PLANAR_REQUIRE_ORACLE(available, reason)                                                                                 \
  do {                                                                                                                           \
    if (!(available)) {                                                                                                          \
      if (::planar::parity::strict_mode()) {                                                                                     \
        FAIL("PLANAR_PARITY_STRICT is set and the oracle is unavailable: " << (reason));                                         \
      }                                                                                                                          \
      SKIP(reason);                                                                                                              \
    }                                                                                                                            \
  } while (false)
