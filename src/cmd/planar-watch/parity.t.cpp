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
// verbs), and `completion <shell>`'s generated script (planar.cliapp.completion
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

TEST_CASE("planar-watch: leaf help pages are exact, and still cost nothing to render", "[cmd][watch][parity]") {
  // TASK 6123 RE-BASELINE. This case used to diff each leaf's `--help`
  // page against the Zig oracle byte for byte. `src/lib/cli`'s help
  // renderer — the thing that produced those bytes — is deleted; CLI11
  // renders help now, and the operator sanctioned the re-baseline. So the
  // page is no longer oracle-comparable and this case no longer claims it
  // is. What it still does is PIN THE EXACT BYTES, captured from the built
  // binary, rather than loosening to a `contains` check: a leaf's page is
  // derived entirely from its own node, so a dropped flag or a mistyped
  // description changes it, and that is the regression worth catching.
  //
  // It also no longer needs the oracle at all, so it runs on a checkout
  // with no zig/ build — strictly more coverage than the SKIP it replaces.
  auto const arena = make_arena("leafhelp");

  auto const version = run_pinned(cpp_bin(), std::vector<std::string>{"version", "--help"}, arena.cpp_root, "version");
  CHECK(version.code == 0);
  CHECK(version.err.empty());
  CHECK(version.out == "Print the planar-watch version, commit, and zig runtime.\n"
                       "\n"
                       "\n"
                       "version [OPTIONS]\n"
                       "\n"
                       "\n"
                       "OPTIONS:\n"
                       "  -h,     --help              Print this help message and exit\n");

  auto const schema = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--help"}, arena.cpp_root, "schema");
  CHECK(schema.code == 0);
  CHECK(schema.err.empty());
  CHECK(schema.out == "Print the full command tree as a JSON catalog (flags, aliases, positionals).\n"
                      "\n"
                      "\n"
                      "schema [OPTIONS]\n"
                      "\n"
                      "\n"
                      "OPTIONS:\n"
                      "  -h,     --help              Print this help message and exit\n");

  // The only ported leaf with a POSITIONAL, so the only one whose page can
  // regress by losing the POSITIONALS section.
  auto const completion = run_pinned(cpp_bin(), std::vector<std::string>{"completion", "--help"}, arena.cpp_root, "completion");
  CHECK(completion.code == 0);
  CHECK(completion.err.empty());
  CHECK(completion.out == "Generate the autocompletion script for the specified shell.\n"
                          "\n"
                          "\n"
                          "completion [OPTIONS] shell\n"
                          "\n"
                          "\n"
                          "POSITIONALS:\n"
                          "  shell REQUIRED              Shell: bash, zsh, or fish\n"
                          "\n"
                          "OPTIONS:\n"
                          "  -h,     --help              Print this help message and exit\n");
}

TEST_CASE("planar-watch parity: completion's two failure paths keep their distinct exit codes",
          "[cmd][watch][parity][exitcode]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-watch)");
  }

  // Handler-level refusal: exit 2, stderr only. STILL A TRUE ORACLE DIFF
  // after task 6123 — this message comes from the HANDLER, not the parser,
  // so the CLI11 swap does not touch it. Left as a byte-for-byte
  // comparison deliberately: it is the control that shows the
  // re-baselining below is confined to parser-produced bytes.
  auto const [cpp_bad, zig_bad] = both("badshell", {"completion", "badshell"});
  CHECK(cpp_bad.code == zig_bad.code);
  CHECK(cpp_bad.code == 2);
  CHECK(cpp_bad.out == zig_bad.out);
  CHECK(cpp_bad.err == zig_bad.err);

  // Parser-level refusal: exit 1, BOTH streams. Two different codes out of
  // one verb — a collapsed exit mapping cannot satisfy both cases, and
  // THAT is what this case exists to prove.
  //
  // The exit CODE is still diffed against the oracle, because the per-binary
  // exit-code table is an operator contract task 6123 was required to
  // preserve. The BYTES are not: CLI11 writes the parse-error wording now
  // (task 6123), so they are pinned against the built binary instead.
  auto const [cpp_missing, zig_missing] = both("noshell", {"completion"});
  CHECK(cpp_missing.code == zig_missing.code);
  CHECK(cpp_missing.code == 1);
  CHECK(cpp_missing.out == "error: shell is required\n");
  CHECK(cpp_missing.err == "error: RequiredError\n");
}

TEST_CASE("planar-watch parity: an unknown verb still exits 1, matching the oracle", "[cmd][watch][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-watch)");
  }

  // As above: the exit CODE is still diffed against the oracle (exit 1
  // here, where the operator binary exits 2 — the divergence task 6123 was
  // required to preserve); the BYTES are CLI11's now and are pinned
  // against the built binary.
  auto const [cpp, zig] = both("unknownverb", {"nosuchverb"});
  CHECK(cpp.code == zig.code);
  CHECK(cpp.code == 1);
  CHECK(cpp.out == "error: planar-watch: The following argument was not expected: nosuchverb\n");
  CHECK(cpp.err == "error: ExtrasError\n");
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
