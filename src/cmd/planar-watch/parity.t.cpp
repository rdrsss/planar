// @file parity.t.cpp
// @brief Differential tests: `planar-watch` (C++) against the Zig reference
// over identical argv in identical pinned scratch environments (plan 996,
// task 6107).
//
// Harness in `../parity_harness.hpp`; see that file for the database-safety
// rules it enforces and why it is a header rather than a target.
//
// ## What is compared
//
// Leaf help pages, both `completion` failure paths (the exit-1/exit-2 pair
// that discriminates this binary's policy), the unknown-verb path, and —
// the case that matters most for this particular binary — that no ported
// invocation creates a database FILE. That last one is the read-only
// invariant observed from OUTSIDE the process, complementing
// `context.t.cpp`'s inside-the-process write-refusal proof.
//
// NOT compared: `version` (inherited `cxx` vs `zig` tag), the ROOT help
// page and `schema` (both describe a tree with three of the oracle's twelve
// verbs), and `completion <shell>`'s generated script (planar.cli.completion
// defers flag-VALUE completion — its own module header says so).
//
// SKIP, not fail, when the oracle is absent (D6).

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::capture;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the Zig reference binary.
/// @return The path.
auto zig_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_BIN};
}

/// @brief True when the reference binary is present to diff against.
/// @return `true` if the oracle exists.
auto oracle_available() -> bool {
  return std::filesystem::exists(zig_bin());
}

/// @brief Run both binaries over `args` in separately-pinned scratch roots.
/// @param tag A short discriminator naming the case.
/// @param args The arguments (excluding argv[0]).
/// @return The two captures, C++ first.
auto both(std::string_view tag, std::vector<std::string> args) -> std::pair<capture, capture> {
  auto const arena = make_arena(tag);
  return {run_pinned(cpp_bin(), args, arena.cpp_root, "cpp"), run_pinned(zig_bin(), args, arena.zig_root, "zig")};
}

} // namespace

TEST_CASE("planar-watch parity: leaf help pages match byte for byte", "[cmd][watch][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-watch)");
  }

  for (auto const& leaf : {"version", "schema", "completion"}) {
    auto const [cpp, zig] = both(leaf, {leaf, "--help"});
    INFO("leaf: " << leaf);
    CHECK(cpp.code == zig.code);
    CHECK(cpp.code == 0);
    CHECK(cpp.out == zig.out);
    CHECK(cpp.err == zig.err);
  }
}

TEST_CASE("planar-watch parity: completion's two failure paths match, codes included", "[cmd][watch][parity][exitcode]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-watch)");
  }

  // Handler-level refusal: exit 2, stderr only.
  auto const [cpp_bad, zig_bad] = both("badshell", {"completion", "badshell"});
  CHECK(cpp_bad.code == zig_bad.code);
  CHECK(cpp_bad.code == 2);
  CHECK(cpp_bad.out == zig_bad.out);
  CHECK(cpp_bad.err == zig_bad.err);

  // Parser-level refusal: exit 1, BOTH streams. Two different codes out of
  // one verb — a collapsed exit mapping cannot satisfy both cases.
  auto const [cpp_missing, zig_missing] = both("noshell", {"completion"});
  CHECK(cpp_missing.code == zig_missing.code);
  CHECK(cpp_missing.code == 1);
  CHECK(cpp_missing.out == zig_missing.out);
  CHECK(cpp_missing.err == zig_missing.err);
}

TEST_CASE("planar-watch parity: an unknown verb matches byte for byte, exit 1 included", "[cmd][watch][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-watch)");
  }

  auto const [cpp, zig] = both("unknownverb", {"nosuchverb"});
  CHECK(cpp.code == zig.code);
  CHECK(cpp.code == 1);
  CHECK(cpp.out == zig.out);
  CHECK(cpp.err == zig.err);
}

TEST_CASE("planar-watch: no ported invocation creates a database file", "[cmd][watch][parity][readonly]") {
  // The read-only invariant observed from outside the process. Runs
  // WITHOUT the oracle too — it is an assertion about this binary, not a
  // comparison — so it stays live even on a checkout with no zig/ build.
  auto const arena = make_arena("nodb");
  for (auto const& argv :
       std::vector<std::vector<std::string>>{{"version"}, {"schema"}, {"completion", "bash"}, {"--help"}, {"nosuchverb"}}) {
    auto const tag = argv.front();
    (void)run_pinned(cpp_bin(), argv, arena.cpp_root, tag);
    INFO("argv: " << tag);
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}
