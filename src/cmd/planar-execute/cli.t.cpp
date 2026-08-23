// @file cli.t.cpp
// @brief `planar-execute`'s argument surface and its two capability locks
// (plan 996, task 6107).
//
// Port target: the `parseRunArgs` unit tests in
// zig/src/cmd/planar-execute/main.zig, plus the "advertises no spawn
// affordance" case from
// zig/integration_tests/capability_boundary_test.zig.
//
// ## The two locks, and what each actually proves
//
//   NO SPAWN AFFORDANCE. `advertises no spawn affordance` below reproduces
//   the Zig integration check, including its five documented exclusions
//   ("workflow" is the Lua file noun; "model" appears in "no model-spawn"
//   prose; "exec" is a substring of "planar-execute"; "spawn" appears in
//   "spawn-free"; "dispatch_table" is a compound covered by the unit
//   lock). Word-boundary matched, not substring matched — the exclusions
//   exist precisely because substring matching produces false positives
//   here. What it proves: the binary's ADVERTISED surface offers no spawn
//   primitive. What it does NOT prove: that no such primitive exists
//   unadvertised — the Zig side pairs it with a runtime host-fn manifest
//   lock (`host.zig`'s ALLOWED_HOST_FNS assertion) that has no counterpart
//   here because the Lua host surface is unported. Stated so nobody reads
//   this test as more than it is.
//
//   NO SQLITE HANDLE. There is no test for this, and that is deliberate:
//   it is enforced at CONFIGURE TIME by cmake/architecture.cmake, which
//   FATALs if this target reaches `planar_db` directly or transitively.
//   The build refusing to generate is stronger evidence than any runtime
//   assertion, because a test can only observe the handle a binary chose
//   to open, whereas the guard makes the edge unrepresentable. See
//   src/cmd/planar-execute/CMakeLists.txt for the naming trap that makes
//   the literal arm of that check actually fire on this target.
//
// ## Break-probes run against this file
//
//   - Added the word "agent" to `usage_text()` -> `advertises no spawn
//     affordance` FAILS naming it. Restored -> green.
//   - Made `parse_run_args` ignore an unrecognised `--flag` instead of
//     refusing -> `an unrecognised long flag is refused` FAILS. Restored
//     -> green.
//   - Reordered `classify` to test `--help` before `run` -> SURVIVOR. The
//     whole suite stayed green, because the two arms match disjoint tokens
//     and no argv reaches both. The claim that the order was observable was
//     simply wrong; the case was rewritten around what IS observable (see
//     `treats \`run --help\` as a failed run` below) and the probe was
//     replaced by the `--help`-recognition mutation, which does fail it.
//   - Dropped the trailing newline from `usage_text()` -> `the usage text
//     is a COMPLETE payload` FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cmd.planar_execute.cli;
import planar.cmd.planar_execute.engine;

namespace {

using planar::cmd::execute::classify;
using planar::cmd::execute::parse_run_args;
using planar::cmd::execute::usage_text;
using planar::cmd::execute::verb;

/// @brief Build an argv (including argv[0]) from a tail.
/// @param tail The arguments after the program name.
/// @return The full argv.
auto argv_of(std::vector<std::string> tail) -> std::vector<std::string> {
  std::vector<std::string> argv{"planar-execute"};
  argv.insert(argv.end(), tail.begin(), tail.end());
  return argv;
}

/// @brief True when `name` occurs in `haystack` as a standalone word.
///
/// Word-boundary matched exactly as the Zig integration check does:
/// preceded and followed by a non-alphanumeric character or a string
/// boundary. Substring matching would fire on "exec" inside
/// "planar-execute", which is why the Zig version excludes it rather than
/// matching loosely.
/// @param haystack The text to scan.
/// @param name The word to look for.
/// @return `true` if present as a whole word.
auto contains_word(std::string_view haystack, std::string_view name) -> bool {
  for (std::size_t pos = 0; (pos = haystack.find(name, pos)) != std::string_view::npos; ++pos) {
    bool const before_ok = pos == 0 || (std::isalnum(static_cast<unsigned char>(haystack[pos - 1])) == 0);
    auto const after     = pos + name.size();
    bool const after_ok  = after >= haystack.size() || (std::isalnum(static_cast<unsigned char>(haystack[after])) == 0);
    if (before_ok && after_ok) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("planar-execute advertises no spawn affordance", "[cmd][execute][capability]") {
  // D7's DENIED_HOST_FNS names, minus the five the Zig check documents as
  // legitimately present in prose. Transcribed from
  // zig/integration_tests/capability_boundary_test.zig's `denied_names`.
  constexpr std::array<std::string_view, 9> denied{"agent", "parallel", "pipeline", "compact", "budget",
                                                   "child", "claude",   "codex",    "headless"};
  for (auto const& name : denied) {
    INFO("denied name found as a word in the advertised surface: " << name);
    CHECK_FALSE(contains_word(usage_text(), name));
  }
}

TEST_CASE("planar-execute's usage text is a COMPLETE payload", "[cmd][execute][cli]") {
  auto const text = usage_text();
  CHECK_FALSE(text.empty());
  // The Zig multiline literal ends with a blank continuation line, i.e. a
  // final newline. Dropping it shortens SIX oracle-compared argv shapes by
  // one byte each.
  CHECK(text.ends_with("payload as JSON on stdout.\n"));
  CHECK(text.starts_with("planar-execute — deterministic, spawn-free Lua workflow engine.\n"));
}

TEST_CASE("planar-execute classifies its top-level argv shapes", "[cmd][execute][cli]") {
  CHECK(classify(argv_of({})) == verb::none);
  CHECK(classify(argv_of({"--help"})) == verb::help);
  CHECK(classify(argv_of({"-h"})) == verb::help);
  CHECK(classify(argv_of({"help"})) == verb::help);
  CHECK(classify(argv_of({"run"})) == verb::run);
  CHECK(classify(argv_of({"bogus"})) == verb::unknown);
  // `--version` is NOT a flag this binary knows — it is an unknown VERB,
  // which is why the oracle answers `planar-execute: unknown verb:
  // --version`. Captured, not assumed.
  CHECK(classify(argv_of({"--version"})) == verb::unknown);
}

TEST_CASE("planar-execute treats `run --help` as a failed run, not a help request", "[cmd][execute][cli]") {
  // SURVIVOR, FOUND AND FIXED. This case originally asserted that
  // `classify` tests `run` BEFORE `--help` and claimed the order was
  // observable. It is not: the two arms match disjoint tokens, so no argv
  // reaches both, and swapping them left the whole suite green. The claim
  // was wrong and the test did not discriminate.
  //
  // What IS observable is the consequence, and it is a real trap: because
  // `run` is a verb rather than a help-bearing command, `planar-execute
  // run --help` reaches `parse_run_args`, where `--help` is an
  // unrecognised long flag — so the oracle prints the usage text and exits
  // 2, NOT 0. A port that taught `parse_run_args` to recognise `--help`
  // would flip that code silently. `parity.t.cpp`'s `run_badflag` shape
  // pins the same thing end to end against the live oracle.
  CHECK(classify(argv_of({"run", "--help"})) == verb::run);
  CHECK_FALSE(parse_run_args(std::vector<std::string>{"--help"}).has_value());
}

TEST_CASE("planar-execute parses a full run invocation", "[cmd][execute][cli]") {
  std::vector<std::string> const args{"wf.lua", "--phase", "setup", "--args", "{\"x\":1}"};
  auto const                     parsed = parse_run_args(args);
  REQUIRE(parsed.has_value());
  CHECK(parsed->workflow == "wf.lua");
  CHECK(parsed->phase == "setup");
  CHECK(parsed->args_json == "{\"x\":1}");
  CHECK(parsed->worktree.empty());
  CHECK(parsed->sandbox_root.empty());
}

TEST_CASE("planar-execute parses the worktree and sandbox-root flags", "[cmd][execute][cli]") {
  std::vector<std::string> const args{"wf.lua", "--phase", "p", "--worktree", "/tmp/wt", "--sandbox-root", "/tmp/sb"};
  auto const                     parsed = parse_run_args(args);
  REQUIRE(parsed.has_value());
  CHECK(parsed->worktree == "/tmp/wt");
  CHECK(parsed->sandbox_root == "/tmp/sb");
}

TEST_CASE("planar-execute refuses every bad-usage shape", "[cmd][execute][cli]") {
  // Each of these is a REFUSAL in the original and each is easy to
  // "improve" into a silent acceptance.
  SECTION("missing --phase") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua"}).has_value());
  }
  SECTION("missing workflow positional") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"--phase", "setup"}).has_value());
  }
  SECTION("no arguments at all") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{}).has_value());
  }
  SECTION("an unrecognised long flag is refused, not ignored") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua", "--phase", "p", "--nope"}).has_value());
  }
  SECTION("a flag with no value is refused, not defaulted to empty") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua", "--phase"}).has_value());
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"wf.lua", "--phase", "p", "--args"}).has_value());
  }
  SECTION("a second bare positional is refused, not an overwrite") {
    CHECK_FALSE(parse_run_args(std::vector<std::string>{"a.lua", "b.lua", "--phase", "p"}).has_value());
  }
}

TEST_CASE("planar-execute reads a workflow file, and refuses one it cannot", "[cmd][execute][engine]") {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_exec_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  auto const present = root / "wf.lua";
  {
    std::ofstream file(present, std::ios::binary);
    file << "function setup() end\n";
  }

  auto const read = planar::cmd::execute::read_workflow(present);
  REQUIRE(read.has_value());
  CHECK(*read == "function setup() end\n");

  CHECK_FALSE(planar::cmd::execute::read_workflow(root / "absent.lua").has_value());
  // A DIRECTORY is a read failure, not an empty workflow. An ifstream over
  // one opens successfully on some platforms and then reads zero bytes,
  // which would look like a valid empty file.
  CHECK_FALSE(planar::cmd::execute::read_workflow(root).has_value());
}

TEST_CASE("planar-execute's run path reports load failure with the oracle's message", "[cmd][execute][engine]") {
  std::ostringstream err;
  auto const         outcome = planar::cmd::execute::run_workflow("x.lua", err);
  CHECK(outcome == planar::cmd::execute::run_outcome::load_failed);
  // Oracle bytes, verbatim. This is the ONE planar-execute failure that is
  // exit 1 rather than 2, which is why its message is pinned literally.
  CHECK(err.str() == "planar-execute: cannot read workflow: x.lua\n");
}
