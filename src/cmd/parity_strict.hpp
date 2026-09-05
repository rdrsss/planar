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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace planar::parity {

/// @brief Single-quote `value` for safe interpolation into a shell command,
/// escaping any embedded single quote.
///
/// A local, header-only copy of the same idea `parity_harness.hpp`'s
/// `shell_quote` implements — not reused from there because that header
/// requires `import std;` (its top comment explains why) while this one
/// deliberately stays plain-`#include` so it can be pulled in ahead of any
/// module import at each call site.
/// @param value The raw string.
/// @return The quoted, shell-safe string.
[[nodiscard]] inline auto shell_quote_path(std::string_view value) -> std::string {
  std::string out = "'";
  for (char const c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

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

/// @brief Whether the oracle binary is present but STALE — built before
/// the most recent committed change under `zig/` (plan 996, task 6414).
///
/// Probed HERE, at test time, not at CMake configure time (plan 996, task
/// 6424 — the configure-time version of this check shipped by 6414 baked
/// the answer into a global compile definition, so rebuilding the oracle
/// with `zig build` did not clear it: the flag stayed TRUE until someone
/// re-ran `cmake --preset debug`, and every oracle-gated case failed on a
/// tree that was actually fine. The remediation text that same check
/// printed said "rebuild with `zig build`", which does not fix the
/// failure it names — a guard that survives its own prescribed fix trains
/// people to distrust it. Moving the comparison here removes the
/// staleness of the staleness check itself: each test process re-reads
/// the oracle's mtime and `zig/`'s last committed change fresh, so a
/// rebuild-only cycle (no reconfigure) is immediately visible. The cost
/// is a `git log` and a `stat` per oracle-gated case — negligible next to
/// the subprocess these tests already spawn to run the oracle itself.
///
/// `PLANAR_ORACLE_ROOT` (see the root `CMakeLists.txt`) is still resolved
/// at configure time and passed through as a compile definition: THAT
/// fact — where the checkout root lives relative to this build — is
/// genuinely configure-time (it depends on whether this is a worktree or
/// the main checkout), unlike staleness, which depends on wall-clock
/// state that changes between configures.
/// @return `true` when the oracle is stale. `false` when it is absent —
/// that case is handled by `PLANAR_REQUIRE_ORACLE`'s `available` check,
/// not here.
[[nodiscard]] inline auto oracle_stale() -> bool {
#if !defined(PLANAR_ORACLE_ROOT)
  return false;
#else
  namespace fs = std::filesystem;

  fs::path const oracle_root{PLANAR_ORACLE_ROOT};
  fs::path const oracle_bin = oracle_root / "zig" / "zig-out" / "bin" / "planar";

  std::error_code ec;
  auto const      oracle_mtime = fs::last_write_time(oracle_bin, ec);
  if (ec) {
    // No oracle binary at all — that is the absent case, not stale.
    return false;
  }

  // `git log -1 --format=%ct -- <pathspecs>` — the unix-epoch commit time
  // of the most recent COMMITTED change under the paths that actually
  // produce `zig/zig-out/bin/planar`. Deliberately NARROWER than the whole
  // `zig/` tree (task 6447): `zig/build.zig` wires the `exe` artifact from
  // `src/cmd/planar/main.zig` through `mod`/`db_mod`/`cli_mod`/`engine_mod`/
  // `runtime_mod`, which in turn pull in `zig/src/`, the vendored C/Zig deps
  // under `zig/vendor/` (sqlite, lua, tree-sitter, tree-sitter-zig, libvaxis,
  // etcli-zig), the dependency manifest `zig/build.zig.zon`, and the two
  // build-time codegen tools `zig/tools/gen_migrations.zig` and
  // `zig/tools/gen_templates.zig` (each spawned by `build.zig` to produce
  // the `migrations`/`templates_embed` modules `db_mod`/`engine_mod`
  // import). `zig/integration_tests/`, `zig/tools/vendor_sync.zig`,
  // `zig/tools/cli_usage_lint.zig`, and `zig/tools/surface_lint.zig` are
  // NOT inputs to this artifact — a commit that only touches those cannot
  // change the built binary, so it must not trip staleness. Shelled rather
  // than linked against libgit2: every other timestamp-vs-git-history
  // comparison in this tree (the CMake configure-time version this
  // replaces) did the same, and a single `popen` per oracle-gated case is
  // the trivial cost documented above.
  std::string const cmd = "git -C " + shell_quote_path(oracle_root.string()) +
                           " log -1 --format=%ct -- "
                           "zig/build.zig zig/build.zig.zon zig/src/ zig/vendor/ "
                           "zig/tools/gen_migrations.zig zig/tools/gen_templates.zig "
                           "2>/dev/null";
  FILE*             pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return false;
  }
  std::string epoch_line;
  char        buf[64];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    epoch_line += buf;
  }
  ::pclose(pipe);
  while (!epoch_line.empty() && (epoch_line.back() == '\n' || epoch_line.back() == '\r')) {
    epoch_line.pop_back();
  }
  if (epoch_line.empty()) {
    // No git history for zig/ (e.g. a release tarball) — nothing to
    // compare against, so nothing can be reported stale.
    return false;
  }

  long long zig_src_epoch = 0;
  try {
    zig_src_epoch = std::stoll(epoch_line);
  } catch (...) {
    return false;
  }

  auto const oracle_sys_time = fs::file_time_type::clock::to_sys(oracle_mtime);
  auto const oracle_epoch =
      std::chrono::duration_cast<std::chrono::seconds>(oracle_sys_time.time_since_epoch()).count();

  return zig_src_epoch > oracle_epoch;
#endif
}

} // namespace planar::parity

/// @brief Skip — or, under `PLANAR_PARITY_STRICT`, FAIL — when the oracle
/// is unavailable. FAIL, unconditionally and never gated behind
/// `PLANAR_PARITY_STRICT`, when the oracle is present but STALE.
///
/// Absent and stale are deliberately NOT the same failure. Absent is a
/// normal, everyday bootstrap state (nobody has run `zig build` yet) and
/// SKIP is the proportionate answer, same as always. Stale means the case
/// WILL run and WILL report a pass or a fail — against a Zig reference
/// that no longer matches the tree, which manufactures false confidence
/// rather than skipping honestly. That is worse than a failing test, so
/// it always fails, the same posture task 6402 gave `schema.t.cpp`'s
/// `[lint-parity]` case (SKIP -> FAIL) for the same reason: a quiet
/// outcome hid a broken dependency edge while ctest still reported green.
///
/// A statement macro rather than a function because Catch2's `SKIP` and
/// `FAIL` both have to expand inside the test body to register against the
/// running case.
#define PLANAR_REQUIRE_ORACLE(available, reason)                                                                                 \
  do {                                                                                                                           \
    if (::planar::parity::oracle_stale()) {                                                                                      \
      FAIL("parity oracle is STALE — zig/ has a committed change newer than the built zig-out binary; rebuild it with "          \
           "`zig build` in zig/ before trusting this comparison");                                                               \
    }                                                                                                                            \
    if (!(available)) {                                                                                                          \
      if (::planar::parity::strict_mode()) {                                                                                     \
        FAIL("PLANAR_PARITY_STRICT is set and the oracle is unavailable: " << (reason));                                         \
      }                                                                                                                          \
      SKIP(reason);                                                                                                              \
    }                                                                                                                            \
  } while (false)
