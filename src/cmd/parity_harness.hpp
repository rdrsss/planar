// @file parity_harness.hpp
// @brief The differential-test harness every `src/cmd/<binary>/parity.t.cpp`
// shares: run a built C++ binary and its Zig reference over identical argv
// in identical pinned scratch environments and compare stdout, stderr and
// exit code (plan 996, task 6107).
//
// ## Why this is a HEADER and not a module or a library
//
// Because a library would be illegal and a module would be pointless.
//
// D18 forbids a `cmd_* -> cmd_*` edge; cmake/architecture.cmake FATALs at
// configure time on one. So the four binaries cannot share layer-3
// vocabulary through a target — not their `context`, not their `exit`, not
// their dispatch, and not this. A shared layer-1 library would be legal by
// the layer numbers but wrong twice over: it would put test-only scaffolding
// in the shipped base-library layer, and — for THIS file specifically — it
// would drag `planar_db` (which the safety guard below needs) into the link
// closure of `planar-execute`'s test target, whose whole invariant is that
// it reaches no SQLite handle. cmake/architecture.cmake would FATAL on that
// too, via the execute-carrier check.
//
// A plain header included by each `parity.t.cpp` creates NO target edge:
// each test target compiles its own copy, exactly as if the code had been
// pasted there, which is what task 6105's `parity.t.cpp` did and what three
// more copies would otherwise do. Nothing that ships includes it.
//
// The one thing that IS deduplicated here is the part that was already
// gotten wrong once and must never be gotten wrong again — the pinned
// environment. See `run_pinned`.
//
// SKIP, not fail, when an oracle binary is absent: `zig/zig-out/bin/*` are
// build artifacts, not checked-in files. Same posture src/lib/db/migrate.t.cpp
// already takes (D6: the zig/ tree is the parity oracle until M10).
#pragma once

#include <sys/wait.h>

// This header is included from module-importing test translation units, so
// it must not `#include` any standard library header — `import std;` at the
// top of the includer provides everything used below.

namespace planar::cmd::parity {

/// @brief One binary's observable output for one invocation.
struct capture {
  int         code = 0; ///< The process exit status.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief Read a whole file as bytes, or the empty string when absent.
/// @param path The file to read.
/// @return The file's contents.
inline auto read_all(const std::filesystem::path& path) -> std::string {
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
inline auto shell_quote(std::string_view value) -> std::string {
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
///
/// DATABASE SAFETY IS THE WHOLE REASON THIS FUNCTION EXISTS IN ONE PLACE.
/// `planar`, `planar-agent` and `planar-watch` all resolve `$PLANAR_DB` and
/// otherwise fall back to `~/.planar/planar.db`; `planar` and the Zig
/// runtime's writable bootstraps apply pending migrations AUTOMATICALLY on
/// first use. Shelling any of them with the inherited environment therefore
/// opens the operator's live database — and the moment a migration lands on
/// this branch, `ctest` would migrate it past the version every installed
/// binary supports and lock every other agent on the machine out. That is
/// not hypothetical: commit 3ec6c37 fixed exactly this defect in two cli
/// parity tests.
///
/// `cd` FIRST, then `env` — and never the other way round. The obvious
/// spelling, `VAR=x cd dir && binary`, silently does NOT export the
/// assignments to `binary` on this platform's `/bin/sh` (verified:
/// `sh -c "FOO=bar cd /tmp && env | grep FOO"` prints nothing). Task 6105's
/// parity file was written that way first, and BOTH binaries fell back to
/// the inherited HOME and wrote ten annotation rows into the operator's live
/// `~/.planar/planar.db` before a mismatch in row ids gave it away.
///
/// That failure was also nearly missed, because the first probe for it —
/// comparing the live database file's mtime before and after — reported
/// "untouched". Under WAL, writes land in the `-wal` sibling and the main
/// `.db` file's mtime does not move. An mtime check on the main file is NOT
/// a valid liveness probe here; a content hash or a row count is.
///
/// A NOTE ON REDIRECTING THE ZIG BINARIES' STDOUT: the Zig runtime's file
/// writer uses POSITIONAL writes, so two Zig invocations appending to the
/// SAME redirect target both start at offset 0 and the second overwrites
/// the first. Every invocation here gets its own freshly-created capture
/// file, keyed by `tag`. Do not "simplify" this into a shared append target.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; `work/proj` is also the working directory.
/// @param tag A discriminator so each invocation gets its own capture files.
/// @return The captured result.
inline auto run_pinned(const std::filesystem::path& bin, std::span<const std::string> args, const std::filesystem::path& work,
                       std::string_view tag) -> capture {
  auto const out_path = work / std::format("{}.out", tag);
  auto const err_path = work / std::format("{}.err", tag);

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
inline auto make_arena(std::string_view tag) -> arena {
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

} // namespace planar::cmd::parity
