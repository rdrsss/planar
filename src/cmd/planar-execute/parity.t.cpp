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

#include "parity_strict.hpp"

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
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-execute)");

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
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-execute)");

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

namespace {

/// @brief Seed both arenas with the same workflow file and run both
/// binaries over it, comparing stdout, stderr and exit code.
/// @param tag A short discriminator naming the case.
/// @param workflow The Lua source.
/// @param extra Arguments after `run wf.lua --phase p`.
/// @return The two captures, C++ first.
auto both_over_workflow(std::string_view tag, std::string_view workflow, std::vector<std::string> const& extra = {})
    -> std::pair<capture, capture> {
  auto const arena = make_arena(tag);
  for (auto const& root : {arena.cpp_root, arena.zig_root}) {
    std::ofstream file(root / "proj" / "wf.lua", std::ios::binary);
    file << workflow;
  }
  std::vector<std::string> args{"run", "wf.lua", "--phase", "p"};
  args.insert(args.end(), extra.begin(), extra.end());
  return {run_pinned(cpp_bin(), args, arena.cpp_root, "cpp"), run_pinned(zig_bin(), args, arena.zig_root, "zig")};
}

} // namespace

TEST_CASE("planar-execute parity: a workflow runs identically in both engines", "[cmd][execute][parity][workflow]") {
  // Task 6107 could not write this case: the Lua half was deferred and this
  // binary exited 64 over any readable workflow. Task 6042 landed the engine,
  // so the deferral test it replaced is gone and the real comparison is here.
  //
  // The cases are chosen for the places two independent implementations
  // diverge WITHOUT either looking wrong on its own — number formatting,
  // object key order, integer width, the stdout/stderr split, and which
  // failures land on which exit code.
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-execute)");

  struct shape {
    std::string_view         tag;
    std::string_view         workflow;
    std::vector<std::string> extra;
    int                      expected;
  };
  std::vector<shape> const shapes{
      // A payload exercising every marshalled type at once. Object key order
      // is the interesting part: Lua's traversal order depends on the
      // per-state hash seed, so an implementation that emitted raw order
      // would differ from the oracle AND from itself run to run.
      {"payload",
       "function p() flow.result({ zebra = 1, apple = 'a\"b\\nc', list = {1, 2, 3}, nested = { deep = true }, empty = {}, "
       "no = false }) end",
       {},
       0},
      // Float spellings. Zig's `{d}` is shortest-round-trip digits with NO
      // exponent, so 1e300 is three hundred and one characters and 3e-7 is
      // `0.0000003`. Both of the obvious C++ formatters get this wrong in a
      // different direction.
      {"floats", "function p() flow.result({ a = 1/3, b = 1e300, c = -0.0, d = 1.0, e = 1e21, f = 3.0e-7, g = 1/0 }) end", {}, 0},
      // Integer width through --args and back out again.
      {"integers",
       "function p() flow.result({ n = ctx.args.n, big = ctx.args.big, t = math.type(ctx.args.n) }) end",
       {"--args", R"({"n":5,"big":9007199254740993})"},
       0},
      // The sandbox, enumerated live. This is the case that would catch a
      // library opened here that the oracle leaves closed.
      {"sandbox",
       "function p() local r = {} for _, n in ipairs({'os','io','debug','coroutine','package','require','load','loadfile',"
       "'dofile','print','pcall'}) do r[n] = (rawget(_G, n) == nil) and 'NIL' or type(rawget(_G, n)) end "
       "r.random = tostring(math.random) flow.result(r) end",
       {},
       0},
      // The five host tables and their exact contents, from a live state.
      {"surface",
       "local function keys(t) local r = {} for k, v in pairs(t) do r[#r+1] = k .. ':' .. type(v) end table.sort(r) "
       "return table.concat(r, ',') end "
       "function p() flow.result({ cli = keys(cli), git = keys(git), fs = keys(fs), flow = keys(flow), ctx = keys(ctx) }) end",
       {},
       0},
      // stdout stays clean while a diagnostic is being written to stderr.
      {"log", "function p() flow.log('hello') flow.result({ok = true}) end", {}, 0},
      // No result at all is still a JSON document.
      {"empty", "function p() end", {}, 0},
      // The four failure stages, each on its own exit code path (all 1 —
      // which is itself the thing being pinned, since only BadUsage is 2).
      {"syntax", "this is not lua", {}, 1},
      {"init", "error('boom')", {}, 1},
      {"raise", "function p() error('kaboom') end", {}, 1},
      {"fail", "function p() flow.fail('nope') end", {}, 1},
      {"sparse", "function p() local t = {} t[1] = 'a' t[3] = 'c' flow.result({v = t}) end", {}, 1},
      // git and fs with no configuration: the refusal message and the exit
      // code, not a fallback to cwd.
      {"unconfigured_git", "function p() git.head_sha() end", {}, 1},
      {"unconfigured_fs", "function p() fs.read('a.txt') end", {}, 1},
  };

  for (auto const& s : shapes) {
    auto const [cpp, zig] = both_over_workflow(s.tag, s.workflow, s.extra);
    INFO("workflow: " << s.tag);
    CHECK(cpp.code == zig.code);
    CHECK(cpp.code == s.expected);
    CHECK(cpp.out == zig.out);
    CHECK(cpp.err == zig.err);
  }
}

TEST_CASE("planar-execute parity: fs confinement behaves identically in both engines", "[cmd][execute][parity][workflow]") {
  PLANAR_REQUIRE_ORACLE(oracle_available(), "Zig oracle not built (zig/zig-out/bin/planar-execute)");

  static constexpr std::string_view k_workflow = R"(
local function try(f, ...)
  local ok, v = pcall(f, ...)
  if not ok then return "ERR" end
  if v == nil then return "OK" end
  return v
end
function p()
  flow.result({
    inside = try(fs.read, "inside.txt"),
    deep = try(fs.read, "sub/deep.txt"),
    parent = try(fs.read, "../outside.txt"),
    absolute = try(fs.read, "/etc/hosts"),
    dot = try(fs.read, "./inside.txt"),
    symlink = try(fs.read, "escape.txt"),
    through_dirlink = try(fs.read, "dirlink/deep.txt"),
    exists_symlink = try(fs.exists, "escape.txt"),
    mkdir_over_symlink = try(fs.mkdir, "dirlink"),
    write_symlink = try(fs.write, "escape.txt", "nope"),
    mkdir_nested = try(fs.mkdir, "made/deeper"),
    write_new = try(fs.write, "made/deeper/out.txt", "written"),
    write_back = try(fs.read, "made/deeper/out.txt"),
  })
end
)";

  // The sandbox root is each arena's own `proj` directory, seeded
  // identically. The symlinked DIRECTORY is the case a text-only `..` check
  // misses: `dirlink/deep.txt` is neither absolute nor dotted.
  auto const      arena = make_arena("confine");
  std::error_code ec;
  for (auto const& root : {arena.cpp_root, arena.zig_root}) {
    auto const proj = root / "proj";
    std::filesystem::create_directories(proj / "sub", ec);
    {
      std::ofstream file(proj / "wf.lua", std::ios::binary);
      file << k_workflow;
    }
    {
      std::ofstream file(proj / "inside.txt", std::ios::binary);
      file << "visible";
    }
    {
      std::ofstream file(proj / "sub" / "deep.txt", std::ios::binary);
      file << "deep";
    }
    // A REAL file one level above the sandbox root, so `../outside.txt`
    // would succeed if the `..` component were ever accepted. Pointing the
    // case at a path that does not exist either way would make it pass for
    // the wrong reason.
    {
      std::ofstream file(root / "outside.txt", std::ios::binary);
      file << "forbidden";
    }
    std::filesystem::create_symlink("/etc/hosts", proj / "escape.txt", ec);
    std::filesystem::create_directory_symlink("sub", proj / "dirlink", ec);
  }

  auto const args = [](std::filesystem::path const& root) {
    return std::vector<std::string>{"run", "wf.lua", "--phase", "p", "--sandbox-root", (root / "proj").string()};
  };
  auto const cpp = run_pinned(cpp_bin(), args(arena.cpp_root), arena.cpp_root, "cpp");
  auto const zig = run_pinned(zig_bin(), args(arena.zig_root), arena.zig_root, "zig");

  CHECK(cpp.code == zig.code);
  CHECK(cpp.code == 0);
  CHECK(cpp.out == zig.out);
  CHECK(cpp.err == zig.err);
  // Not vacuous: the run really did read the file it was allowed to read and
  // refuse the ones it was not, rather than erroring out early and matching
  // on two identical failures.
  CHECK(cpp.out.contains("\"inside\":\"visible\""));
  CHECK(cpp.out.contains("\"symlink\":\"ERR\""));
  CHECK(cpp.out.contains("\"absolute\":\"ERR\""));
  CHECK(cpp.out.contains("\"parent\":\"ERR\""));
  // And neither binary followed the link out of the sandbox.
  CHECK(std::filesystem::read_symlink(arena.cpp_root / "proj" / "escape.txt") == std::filesystem::path{"/etc/hosts"});
  CHECK(std::filesystem::read_symlink(arena.zig_root / "proj" / "escape.txt") == std::filesystem::path{"/etc/hosts"});
}

TEST_CASE("planar-execute: ctx.brief is the one declared divergence from the oracle", "[cmd][execute][parity][workflow]") {
  // Declared in src/lib/engine/execute/CMakeLists.txt and asserted here so it
  // cannot rot into a silent wrong answer: the oracle compiles a brief, this
  // binary reports that the compiler is unported. Everything else on the host
  // surface is ported.
  auto const arena = make_arena("brief");
  {
    std::ofstream file(arena.cpp_root / "proj" / "wf.lua", std::ios::binary);
    file << "function p() ctx.brief({plan_id = 1, task_id = 1, claim_token = 'x', problem_statement = 'y'}) end";
  }
  std::vector<std::string> const args{"run", "wf.lua", "--phase", "p"};
  auto const                     got = run_pinned(cpp_bin(), args, arena.cpp_root, "brief");
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err.contains("the brief compiler is not ported yet"));
}
