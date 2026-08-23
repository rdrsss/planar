// @file parity.t.cpp
// @brief Differential tests: run the built C++ `planar-agent` and the Zig
// reference over identical argv in identical pinned scratch environments,
// and require identical stdout, stderr and exit code (plan 996, task 6107).
//
// The harness lives in `../parity_harness.hpp` — a plain header, included
// rather than linked, because D18 forbids a `cmd_* -> cmd_*` edge and a
// shared layer-1 library for test scaffolding would be worse. That file
// documents the database-safety rules it enforces (`cd` then `env`, never
// `VAR=x cd dir && binary`; per-invocation capture files because Zig's
// writer uses POSITIONAL writes).
//
// A live diff is stronger evidence than a transcribed expectation, because
// a transcription can be wrong and a diff cannot. M9's parity gate rests on
// exactly this shape.
//
// ## What is compared, and what deliberately is not
//
// COMPARED: every argv shape whose output is derived entirely from a ported
// node — leaf help pages, and every parse-failure path (which is where this
// binary's exit-code divergence lives).
//
// NOT COMPARED, each for a stated reason:
//
//   `version`   The `cxx <compiler>` vs `zig <version>` divergence is
//               inherited from `planar.cli.version` and shared with the
//               operator binary. Shape is pinned in handlers.t.cpp.
//   `--help`    The ROOT page lists this tree's two verbs against the
//               oracle's eighteen. Not comparable until the verbs land.
//   `schema`    Same: the catalog is a description of the tree.
//
// SKIP, not fail, when the oracle is absent: `zig/zig-out/bin/planar-agent`
// is a build artifact, not a checked-in file (D6).

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

/// @brief Run both binaries over `args` in freshly-seeded, separately-pinned
/// scratch roots.
/// @param tag A short discriminator naming the case.
/// @param args The arguments (excluding argv[0]).
/// @return The two captures, C++ first.
auto both(std::string_view tag, std::vector<std::string> args) -> std::pair<capture, capture> {
  auto const arena = make_arena(tag);
  return {run_pinned(cpp_bin(), args, arena.cpp_root, "cpp"), run_pinned(zig_bin(), args, arena.zig_root, "zig")};
}

} // namespace

TEST_CASE("the pinned environment actually reaches planar-agent", "[cmd][agent][parity][safety]") {
  // A guard on the HARNESS, not on the binary, and it exists because this
  // harness's ancestor got it wrong once and wrote ten rows into the
  // operator's live database. If the environment stopped reaching the
  // child, `$PLANAR_DB` would fall back to `$HOME/.planar/planar.db` — so
  // this asserts the child SEES the pinned value.
  auto const                     arena = make_arena("envguard");
  std::vector<std::string> const args{"-c", "printf '%s' \"$PLANAR_DB\""};
  auto const                     got = run_pinned("/bin/sh", args, arena.cpp_root, "probe");
  CHECK(got.code == 0);
  CHECK(got.out == (arena.cpp_root / "planar.db").string());
}

TEST_CASE("planar-agent parity: parse failures match byte for byte, exit 1 included", "[cmd][agent][parity][exitcode]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  // The whole reason this binary needed its own exit module. If the port
  // had reused `planar`'s policy these would come back 2 and this case
  // would fail on the code alone, before any byte comparison.
  auto const [cpp_verb, zig_verb] = both("unknownverb", {"nosuchverb"});
  CHECK(cpp_verb.code == zig_verb.code);
  CHECK(cpp_verb.code == 1);
  CHECK(cpp_verb.out == zig_verb.out);
  CHECK(cpp_verb.err == zig_verb.err);

  auto const [cpp_flag, zig_flag] = both("unknownflag", {"version", "--badflag"});
  CHECK(cpp_flag.code == zig_flag.code);
  CHECK(cpp_flag.code == 1);
  CHECK(cpp_flag.out == zig_flag.out);
  CHECK(cpp_flag.err == zig_flag.err);
}

TEST_CASE("planar-agent parity: leaf help pages match byte for byte", "[cmd][agent][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  // A leaf's help page is derived entirely from its own node — name, desc,
  // long_desc, flags, positionals — so a leaf ported at all is ported
  // completely, and a transcription slip in `tree.cpp` shows up here.
  for (auto const& leaf : {"version", "schema"}) {
    auto const [cpp, zig] = both(leaf, {leaf, "--help"});
    INFO("leaf: " << leaf);
    CHECK(cpp.code == zig.code);
    CHECK(cpp.code == 0);
    CHECK(cpp.out == zig.out);
    CHECK(cpp.err == zig.err);
  }
}

TEST_CASE("planar-agent parity: version diverges only in the runtime tag", "[cmd][agent][parity]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  auto const [cpp, zig] = both("version", {"version"});
  CHECK(cpp.code == zig.code);
  CHECK(cpp.code == 0);
  CHECK(cpp.err.empty());
  CHECK(zig.err.empty());

  // Not byte-equal, and this pins exactly HOW MUCH differs: the binary
  // name, sha and date tokens are identical; the runtime tag is not. A
  // divergence that grew beyond that would be a real port bug hiding
  // behind an "expected to differ" comment.
  auto const split = [](std::string_view line) {
    std::vector<std::string> fields;
    for (auto const part : std::views::split(line, ' ')) {
      fields.emplace_back(std::string_view{part});
    }
    return fields;
  };
  auto const cpp_fields = split(std::string_view{cpp.out}.substr(0, cpp.out.size() - 1));
  auto const zig_fields = split(std::string_view{zig.out}.substr(0, zig.out.size() - 1));
  REQUIRE(cpp_fields.size() >= 4);
  REQUIRE(zig_fields.size() == 5);
  CHECK(cpp_fields[0] == zig_fields[0]);
  CHECK(cpp_fields[0] == "planar-agent");
  CHECK(cpp_fields[1] == zig_fields[1]);
  CHECK(cpp_fields[2] == zig_fields[2]);
  CHECK(cpp_fields[3] == "cxx");
  CHECK(zig_fields[3] == "zig");

  // AND the field COUNTS differ, which `planar.cli.version`'s header
  // claims they do not — see this binary's handlers.t.cpp for the full
  // finding. Asserted here against the LIVE oracle rather than a
  // transcription, which is the strongest form the observation takes:
  // `Clang 22.1.8` carries a space of its own, so the C++ line has six
  // whitespace-separated tokens against the oracle's five. A script that
  // splits on whitespace and indexes the last field gets a different
  // answer from each binary.
  CHECK(cpp_fields.size() == 6);
  CHECK(cpp_fields.size() != zig_fields.size());
}

TEST_CASE("planar-agent parity: no ported invocation creates a database", "[cmd][agent][parity][safety]") {
  if (!oracle_available()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-agent)");
  }

  // The consumer policy's most operator-visible consequence, checked
  // end-to-end rather than at the context level: none of the ported verbs
  // opens SQLite, so `$PLANAR_DB` must not exist afterwards. This is also
  // the assertion that would catch an eager open added to `main`.
  auto const arena = make_arena("nodb");
  for (auto const& argv : std::vector<std::vector<std::string>>{{"version"}, {"schema"}, {"--help"}, {"nosuchverb"}}) {
    auto const tag = argv.front();
    (void)run_pinned(cpp_bin(), argv, arena.cpp_root, tag);
    INFO("argv: " << tag);
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}
