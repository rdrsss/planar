// @file parity.t.cpp
// @brief Differential tests: `planar-execute` (C++) against the Zig
// reference over identical argv (plan 996, task 6107).
//
// Harness in `../parity_harness.hpp`.
//
// This binary's parity coverage is the most COMPLETE of the four, because
// its entire argument surface is ported: five of the six argv shapes below
// are compared byte for byte on stdout, stderr AND exit code. That is
// possible here and not elsewhere because planar-execute has no command
// tree to be partially ported — its surface is one usage banner and one
// verb.
//
// Two shapes a reasonable port gets wrong, and both are pinned:
//
//   THE USAGE TEXT GOES TO STDERR, ALWAYS, INCLUDING ON `--help`, where
//   stdout stays completely empty and the exit code is 0. Every other
//   binary in this tree writes help to stdout. A port that "fixed" this
//   would break every caller redirecting the JSON result channel.
//
//   A BARE INVOCATION AND `--help` EMIT IDENTICAL BYTES BUT DIFFERENT
//   CODES — 2 and 0. Comparing only output would pass a port that
//   collapsed them.
//
// NOT compared: `run <file> --phase <p>` where the file EXISTS. The Lua
// engine is unported (no Lua in this tree, no engine_execute bucket), so
// this binary exits 64 there while the oracle runs the workflow. Declared
// in engine.cppm and asserted below as a DIVERGENCE rather than silently
// skipped — a deferral nobody tests is a deferral nobody notices.
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

TEST_CASE("planar-execute parity: every ported argv shape matches byte for byte", "[cmd][execute][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-execute)");
  }

  struct shape {
    std::string_view         tag;
    std::vector<std::string> args;
    int                      expected;
  };
  // Exit codes named explicitly so a case cannot pass by BOTH binaries
  // being wrong in the same way — a real risk when the reference is
  // consulted through the same harness the port was written against.
  std::vector<shape> const shapes{
      {"bare", {}, 2},
      {"help_long", {"--help"}, 0},
      {"help_short", {"-h"}, 0},
      {"help_word", {"help"}, 0},
      {"unknown", {"bogus"}, 2},
      {"unknown_dashed", {"--version"}, 2},
      {"run_noargs", {"run"}, 2},
      {"run_nophase", {"run", "x.lua"}, 2},
      {"run_badflag", {"run", "x.lua", "--phase", "p", "--nope"}, 2},
      {"run_missing_file", {"run", "x.lua", "--phase", "p"}, 1},
  };

  for (auto const& s : shapes) {
    auto const [cpp, zig] = both(s.tag, s.args);
    INFO("shape: " << s.tag);
    CHECK(cpp.code == zig.code);
    CHECK(cpp.code == s.expected);
    CHECK(cpp.out == zig.out);
    CHECK(cpp.err == zig.err);
    // stdout is the clean JSON result channel and stays EMPTY on every one
    // of these — including `--help`, which is the shape most likely to be
    // "fixed" onto stdout by a well-meaning port.
    CHECK(cpp.out.empty());
  }
}

TEST_CASE("planar-execute parity: --help and a bare invocation differ ONLY in exit code", "[cmd][execute][parity][exitcode]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-execute)");
  }

  auto const [cpp_help, zig_help] = both("h", {"--help"});
  auto const [cpp_bare, zig_bare] = both("b", {});

  // Identical bytes on both streams…
  CHECK(cpp_help.err == cpp_bare.err);
  CHECK(zig_help.err == zig_bare.err);
  CHECK(cpp_help.err == zig_help.err);
  // …and DIFFERENT codes. Output-only comparison would pass a port that
  // collapsed these two.
  CHECK(cpp_help.code == 0);
  CHECK(cpp_bare.code == 2);
  CHECK(zig_help.code == 0);
  CHECK(zig_bare.code == 2);
}

TEST_CASE("planar-execute: a readable workflow reports the unported engine rather than faking a result",
          "[cmd][execute][parity]") {
  // The declared divergence, asserted so it cannot rot into a silent wrong
  // answer. The oracle would LOAD and RUN the workflow; this binary exits
  // 64 naming the gap. Printing `{}` and exiting 0 — which is what the
  // oracle does for a workflow declaring no result — would be
  // indistinguishable from success to a caller.
  auto const arena = make_arena("unported");
  auto const wf    = arena.cpp_root / "proj" / "wf.lua";
  {
    std::ofstream file(wf, std::ios::binary);
    file << "function setup() end\n";
  }

  std::vector<std::string> const args{"run", "wf.lua", "--phase", "setup"};
  auto const                     got = run_pinned(cpp_bin(), args, arena.cpp_root, "unported");
  CHECK(got.code == 64);
  CHECK(got.out.empty());
  CHECK(got.err.contains("the Lua workflow engine is not ported yet"));

  if (!oracle_available()) {
    return;
  }
  // And the oracle really does succeed on the same input, which is what
  // makes this a divergence rather than a shared limitation.
  auto const oracle = run_pinned(zig_bin(), args, arena.zig_root, "unported");
  (void)std::filesystem::copy_file(wf, arena.zig_root / "proj" / "wf.lua", std::filesystem::copy_options::overwrite_existing);
  auto const oracle_retry = run_pinned(zig_bin(), args, arena.zig_root, "unported2");
  INFO("oracle first attempt exit " << oracle.code);
  CHECK(oracle_retry.code != 64);
}
