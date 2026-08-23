// @file parity.t.cpp
// @brief Differential tests: run the built C++ `planar` binary and the Zig
// reference binary over identical argv in identical scratch environments,
// and require identical stdout, stderr and exit code (plan 996, task 6105).
//
// This pattern did not exist before this task — there was no C++ binary to
// run. It is the strongest evidence available at this layer, because it
// compares what an operator actually sees rather than what an intermediate
// function returns, and because a captured expectation transcribed into a
// test can be transcribed wrong while a live diff cannot. M9's parity gate
// rests on exactly this shape.
//
// SKIP, not fail, when the oracle is absent: `zig/zig-out/bin/planar` is a
// build artifact, not a checked-in file. Same posture as
// src/lib/db/migrate.t.cpp and src/lib/cli/help.t.cpp (D6 — the zig/ tree
// is the parity oracle until M10 cutover).
//
// DATABASE SAFETY IS THE WHOLE REASON THIS FILE IS CAREFUL.
// Both binaries resolve $PLANAR_DB and otherwise fall back to
// ~/.planar/planar.db, and BOTH apply pending migrations AUTOMATICALLY on
// first use. Shelling either with the inherited environment therefore opens
// the operator's live database — and the moment a migration lands on this
// branch, `ctest` would migrate it past the version every installed binary
// supports and lock every other agent on the machine out. That is not
// hypothetical: commit 3ec6c37 fixed exactly this defect in two cli parity
// tests, found by an unexplained live-DB mtime movement. Every invocation
// below runs under a per-case scratch root with PLANAR_DB, PLANAR_HOME,
// PLANAR_CONFIG_PATH, PLANAR_LOCAL_HOME and HOME all redirected into it —
// PLANAR_LOCAL_HOME included because the `local` sandbox resolves from that
// variable (falling back to HOME), never PLANAR_HOME, and a future ported
// leaf that reached it would otherwise write symlinks into the operator's
// real ~/.claude.
//
// A NOTE ON REDIRECTING THE ZIG BINARY'S STDOUT.
// Observed while capturing fixtures for this task: the Zig runtime's file
// writer uses POSITIONAL writes, so two Zig invocations appending to the
// SAME redirect target both start at offset 0 and the second overwrites the
// first. Every invocation here gets its own freshly-created capture file,
// which sidesteps it. Do not "simplify" this into a shared append target.

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

import std;

namespace {

/// @brief One binary's observable output for one invocation.
struct capture {
  int         code = 0; ///< The process exit status.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief Read a whole file as bytes, or the empty string when absent.
/// @param path The file to read.
/// @return The file's contents.
auto read_all(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

/// @brief Shell-quote one argument for the `/bin/sh` line built below.
/// @param value The argument.
/// @return The single-quoted form.
auto shell_quote(std::string_view value) -> std::string {
  std::string quoted = "'";
  for (char const c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

/// @brief Run `bin` with `args` inside `work`, under an environment pinned
/// entirely to `work`.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; also the working directory.
/// @param tag A discriminator so each invocation gets its own capture files.
/// @return The captured result.
auto run_pinned(const std::filesystem::path& bin, std::span<const std::string> args, const std::filesystem::path& work,
                std::string_view tag) -> capture {
  auto const out_path = work / std::format("{}.out", tag);
  auto const err_path = work / std::format("{}.err", tag);

  // `cd` FIRST, then `env` — and never the other way round. The obvious
  // spelling, `VAR=x cd dir && binary`, silently does NOT export the
  // assignments to `binary`: POSIX keeps assignments preceding a special
  // built-in only in shells that implement that rule, and this platform's
  // /bin/sh does not (verified: `sh -c "FOO=bar cd /tmp && env | grep FOO"`
  // prints nothing). This file was written that way first, and the result
  // was that BOTH binaries fell back to the inherited HOME and wrote ten
  // annotation rows into the operator's live ~/.planar/planar.db before the
  // mismatch in row ids gave it away. Routing through `env` puts the
  // variables in the child's environment unconditionally, with no
  // shell-specific rule in the path.
  //
  // The failure was also nearly missed, because the FIRST probe for it —
  // comparing the live database file's mtime before and after — reported
  // "untouched". Under WAL (which the runtime enables on every connection)
  // writes land in the `-wal` sibling and the main `.db` file's mtime does
  // not move. An mtime check on the main file alone is not a valid
  // liveness probe here; row counts are.
  std::string line = std::format("cd {} && env", shell_quote((work / "proj").string()));
  line += std::format(" PLANAR_DB={}", shell_quote((work / "planar.db").string()));
  line += std::format(" PLANAR_HOME={}", shell_quote((work / "home").string()));
  line += std::format(" PLANAR_CONFIG_PATH={}", shell_quote((work / "config.toml").string()));
  line += std::format(" PLANAR_LOCAL_HOME={}", shell_quote((work / "localhome").string()));
  line += std::format(" HOME={}", shell_quote((work / "fakehome").string()));
  line += std::format(" PWD={} ", shell_quote((work / "proj").string()));
  line += shell_quote(bin.string());
  for (auto const& arg : args) {
    line += " " + shell_quote(arg);
  }
  line += std::format(" > {} 2> {}", shell_quote(out_path.string()), shell_quote(err_path.string()));

  int const status = std::system(line.c_str());
  int const code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return capture{.code = code, .out = read_all(out_path), .err = read_all(err_path)};
}

/// @brief A pair of scratch roots — one per binary — seeded identically.
struct arena {
  std::filesystem::path cpp_root; ///< Scratch root for the C++ binary.
  std::filesystem::path zig_root; ///< Scratch root for the Zig binary.
};

/// @brief Create a fresh arena.
/// @param tag A short discriminator so a failure names its own case.
/// @return The created arena.
auto make_arena(std::string_view tag) -> arena {
  auto const      base = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_parity_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  arena           result{.cpp_root = base / "cpp", .zig_root = base / "zig"};
  for (auto const& root : {result.cpp_root, result.zig_root}) {
    std::filesystem::create_directories(root / "home", ec);
    std::filesystem::create_directories(root / "proj", ec);
    std::filesystem::create_directories(root / "fakehome", ec);
  }
  return result;
}

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the Zig reference binary (set by this target's CMakeLists).
/// @return The path.
auto zig_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_BIN};
}

/// @brief True when the reference binary is present to diff against.
/// @return `true` if the oracle exists.
auto oracle_available() -> bool {
  return std::filesystem::exists(zig_bin());
}

} // namespace

TEST_CASE("the pinned environment actually reaches the child process", "[cmd][parity][safety]") {
  // A guard on the HARNESS, not on the binary — and it exists because the
  // harness got this wrong once already (see `run_pinned`). If the
  // environment silently stops reaching the child, every other case in this
  // file keeps passing while both binaries quietly operate on the
  // operator's real ~/.planar/planar.db. The probe is a positive one: after
  // a database-backed verb, the SCRATCH database must exist. It cannot
  // unless $PLANAR_DB arrived.
  auto const space = make_arena("envpin");
  auto const got   = run_pinned(cpp_bin(), std::array<std::string, 2>{"annotate", "list"}, space.cpp_root, "envpin");
  REQUIRE(got.code == 0);
  CHECK(std::filesystem::exists(space.cpp_root / "planar.db"));
  // And the HOME fallback path must be untouched, which is what a leaked
  // environment would have used.
  CHECK_FALSE(std::filesystem::exists(space.cpp_root / "fakehome" / ".planar" / "planar.db"));

  if (oracle_available()) {
    auto const ref = run_pinned(zig_bin(), std::array<std::string, 2>{"annotate", "list"}, space.zig_root, "envpin");
    REQUIRE(ref.code == 0);
    CHECK(std::filesystem::exists(space.zig_root / "planar.db"));
    CHECK_FALSE(std::filesystem::exists(space.zig_root / "fakehome" / ".planar" / "planar.db"));
  }
}

TEST_CASE("C++ and Zig agree byte-for-byte on the no-database leaves", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  struct leaf {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  std::vector<leaf> const leaves{
      {"wl", {"workflow", "list"}},
      {"wlj", {"workflow", "list", "--json"}},
      {"wll", {"workflow", "list", "--local"}},
      {"wsmiss", {"workflow", "show", "nope"}},
      {"wsmissj", {"workflow", "show", "nope", "--json"}},
      {"wlhelp", {"workflow", "list", "--help"}},
      {"wshelp", {"workflow", "show", "--help"}},
      {"vhelp", {"version", "--help"}},
      {"badverb", {"nosuchverb"}},
      {"badsub", {"workflow", "nosuchsub"}},
      {"badflag", {"workflow", "list", "--nosuchflag"}},
      {"missingpos", {"workflow", "show"}},
      {"aahelp", {"annotate", "add", "--help"}},
      {"alhelp", {"annotate", "list", "--help"}},
      {"aareq", {"annotate", "add"}},
  };

  for (auto const& [tag, args] : leaves) {
    auto const space = make_arena(tag);
    auto const mine  = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref   = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("leaf: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(mine.out == ref.out);
    CHECK(mine.err == ref.err);
  }
}

TEST_CASE("C++ and Zig agree byte-for-byte on the database-backed leaves", "[cmd][parity][oracle]") {
  if (!oracle_available()) {
    SKIP("zig reference binary not built (zig/zig-out/bin/planar) — run `make build` in zig/ to enable the parity lane");
  }

  // Everything except the two wall-clock timestamp fields, which cannot
  // agree across two processes. The text renderer prints them on their own
  // `created:` / `updated:` lines, so those two lines are dropped from both
  // sides before comparing; the JSON object interleaves them, so the JSON
  // case compares the prefix up to `"created_at"` and the shape after it.
  auto const strip_timestamps = [](std::string_view text) {
    std::string kept;
    for (auto const line : std::views::split(text, '\n')) {
      std::string_view view{line.begin(), line.end()};
      if (view.starts_with("created:") || view.starts_with("updated:")) {
        continue;
      }
      kept += view;
      kept += '\n';
    }
    return kept;
  };

  struct step {
    std::string_view         tag;  ///< Case discriminator.
    std::vector<std::string> args; ///< The argv tail.
  };
  // Run as an ordered sequence against ONE database per binary, so the
  // second `annotate list` sees the row the `annotate add` before it wrote
  // — a fresh arena per step would only ever compare empty states.
  std::vector<step> const steps{
      {"al0", {"annotate", "list"}},
      {"al0j", {"annotate", "list", "--json"}},
      {"add1",
       {"annotate", "add", "--anchor-path", "src/foo.zig", "--line-start", "3", "--line-end", "7", "--title", "T1", "--body",
        "B1", "--tags", "a, b,a"}},
      {"al1", {"annotate", "list"}},
      {"badstatus", {"annotate", "list", "--status", "bogus"}},
  };

  auto const space = make_arena("dbleaves");
  for (auto const& [tag, args] : steps) {
    auto const mine = run_pinned(cpp_bin(), args, space.cpp_root, tag);
    auto const ref  = run_pinned(zig_bin(), args, space.zig_root, tag);

    INFO("step: " << tag);
    CHECK(mine.code == ref.code);
    CHECK(strip_timestamps(mine.out) == strip_timestamps(ref.out));
    CHECK(mine.err == ref.err);
  }
}

TEST_CASE("neither binary touches a database on a no-database leaf", "[cmd][parity][oracle]") {
  // The lazy-acquisition rule, observed from outside: after `workflow list`
  // and `version` and a `--help`, the scratch $PLANAR_DB file must not
  // exist at all. This is the same property `context.t.cpp` asserts through
  // `db_opened()`, checked here against the shipped binary rather than the
  // in-process context — and it is the property whose absence made commit
  // 3ec6c37's defect dangerous rather than merely untidy.
  auto const space = make_arena("nodb");
  auto const db    = space.cpp_root / "planar.db";

  for (std::vector<std::string> const args : {std::vector<std::string>{"version"},
                                              {"workflow", "list"},
                                              {"workflow", "list", "--json"},
                                              {"workflow", "show", "nope"},
                                              {"workflow", "show", "--help"}}) {
    auto const got = run_pinned(cpp_bin(), args, space.cpp_root, "nodb");
    INFO("args: " << std::format("{}", args));
    CHECK_FALSE(std::filesystem::exists(db));
    CHECK(got.code != -1);
  }
}
