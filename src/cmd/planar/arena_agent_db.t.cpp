// @file arena_agent_db.t.cpp
// @brief Every entry point that runs a from-source binary pins
// `PLANAR_AGENT_DB` to a scratch path (plan 1080, task 6996; decision 1181).
//
// ## Why a dedicated file
//
// Decision 1181 gives agent state its own database, `~/.planar/agent.db`,
// overridable by `PLANAR_AGENT_DB`. `src/lib/db/agentdb.cppm` (task 6994)
// resolves that path and MIGRATES the file on first open, exactly as the
// main database does. So every hazard `parity_harness.hpp` spends its
// header on — a from-source binary run with an inherited environment
// migrating the operator's live store past every installed binary — now
// has a second target, and every place that runs a built binary has to pin
// the second path as deliberately as it pins `PLANAR_DB`.
//
// There are five such places: `run_pinned` and `launch_pinned_detached` in
// the harness (one env map, `pinned_env`, feeds both), the three gate
// scripts (`scripts/exit-code-contract.sh`, `scripts/surface-snapshot.sh`,
// `scripts/coverage-check.sh`) and the `make smoke` recipe. This file
// covers all five from the outside: it runs each with a RECORDING WRAPPER
// in place of the binary and asserts on the environment the wrapper saw.
// Nothing here runs a real `planar` at all, so nothing here can reach
// `~/.planar` even when an assertion below is wrong.
//
// ## Why the fallback is not good enough
//
// Today every entry point already redirects `HOME`, and the runtime's
// fallback (`$HOME/.planar/agent.db`) therefore already lands in scratch.
// That containment is INCIDENTAL, in exactly the sense the harness header
// uses the word for the workbench root: it survives only as long as nobody
// trims the env map, and the failure mode when it breaks is a suite writing
// into the operator's live agent store while every assertion still passes.
// The assertions below therefore check the VARIABLE, not the fallback: an
// invocation whose `PLANAR_AGENT_DB` is unset fails, however good its `HOME`.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

import std;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::await_sentinel;
using planar::cmd::parity::launch_pinned_detached;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::read_all;
using planar::cmd::parity::run_pinned;
using planar::cmd::parity::shell_quote;

/// @brief The repository root, derived from this file's own compile-time
/// path (`src/cmd/planar/<this>.t.cpp`). No test target carries a
/// source-root define, and the build directory is not guaranteed to sit
/// under the checkout.
/// @return The root; the case fails when it does not hold a `Makefile`.
auto repo_root() -> std::filesystem::path {
  auto const root = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path();
  INFO("derived repository root: " << root.string());
  REQUIRE(std::filesystem::exists(root / "Makefile"));
  REQUIRE(std::filesystem::exists(root / "scripts" / "exit-code-contract.sh"));
  return root;
}

/// @brief What one recorded invocation of a wrapper saw.
struct seen {
  std::string binary;   ///< The wrapper's own basename.
  std::string agent_db; ///< `$PLANAR_AGENT_DB`, or the literal `<unset>`.
  std::string db;       ///< `$PLANAR_DB`, or the literal `<unset>`.
};

/// @brief Write an executable `sh` wrapper named `name` under `dir` that
/// appends one tab-separated `seen` row to `record` and exits 0.
/// @param dir The directory that stands in for a `bin/`.
/// @param name The binary name to impersonate.
/// @param record The file every invocation appends to.
void write_wrapper(const std::filesystem::path& dir, std::string_view name, const std::filesystem::path& record) {
  std::filesystem::create_directories(dir);
  auto const  path = dir / name;
  std::string body = "#!/bin/sh\n";
  body += std::format("printf '%s\\t%s\\t%s\\n' \"$(basename \"$0\")\" \"${{PLANAR_AGENT_DB-<unset>}}\" "
                      "\"${{PLANAR_DB-<unset>}}\" >> {}\n",
                      shell_quote(record.string()));
  body += "exit 0\n";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                         std::filesystem::perms::others_exec);
}

/// @brief Parse the rows `write_wrapper`'s scripts appended.
/// @param record The record file.
/// @return One entry per invocation, in order.
auto rows(const std::filesystem::path& record) -> std::vector<seen> {
  std::vector<seen>  out;
  std::istringstream in(read_all(record));
  std::string        line;
  while (std::getline(in, line)) {
    auto const first  = line.find('\t');
    auto const second = line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
      continue;
    }
    out.push_back(seen{
        .binary = line.substr(0, first), .agent_db = line.substr(first + 1, second - first - 1), .db = line.substr(second + 1)});
  }
  return out;
}

/// @brief Run a shell line, discarding its output, and return its status.
/// @param line The line.
/// @return The exit status as `std::system` reports it.
auto shell(const std::string& line) -> int {
  return std::system(line.c_str());
}

/// @brief The rule every recorded invocation must satisfy: the agent
/// database is pinned, absolute, sits BESIDE the pinned `PLANAR_DB` (so it
/// lives in the same scratch directory that entry point already owns and
/// deletes), and is nowhere under the real `~/.planar`.
/// @param label Which entry point produced the rows.
/// @param seen_rows The rows.
void require_pinned_beside_db(std::string_view label, const std::vector<seen>& seen_rows) {
  INFO("entry point: " << label);
  REQUIRE_FALSE(seen_rows.empty());
  char const* home      = std::getenv("HOME");
  auto const  real_home = std::filesystem::path{home == nullptr ? "" : home} / ".planar";
  for (auto const& row : seen_rows) {
    INFO("invocation of " << row.binary << ": PLANAR_AGENT_DB=" << row.agent_db << " PLANAR_DB=" << row.db);
    REQUIRE(row.agent_db != "<unset>");
    REQUIRE(row.db != "<unset>");
    auto const agent = std::filesystem::path{row.agent_db};
    auto const db    = std::filesystem::path{row.db};
    REQUIRE(agent.is_absolute());
    REQUIRE(agent.parent_path() == db.parent_path());
    REQUIRE(agent.filename() == "agent.db");
    REQUIRE_FALSE(row.agent_db.starts_with(real_home.string()));
  }
}

/// @brief The `PLANAR_AGENT_DB=...` line in an `env(1)` dump, if any.
/// @param dump The captured stdout of `/usr/bin/env`.
/// @return The value, or nullopt when the variable was not exported.
auto agent_db_from_env_dump(std::string_view dump) -> std::optional<std::string> {
  std::istringstream in{std::string{dump}};
  std::string        line;
  while (std::getline(in, line)) {
    if (line.starts_with("PLANAR_AGENT_DB=")) {
      return line.substr(std::string_view{"PLANAR_AGENT_DB="}.size());
    }
  }
  return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Happy path — the arena pins the agent database
// ---------------------------------------------------------------------------

TEST_CASE("arena: run_pinned exports PLANAR_AGENT_DB under the arena root", "[arena][agentdb]") {
  auto const arena = make_arena("agentdb_run");
  // `/usr/bin/env` with no arguments prints the environment it was handed.
  // No Planar binary opens the agent database yet (M1+ do), so the
  // assertion is on the exported variable and where it points, not on a
  // file appearing.
  auto const got = run_pinned("/usr/bin/env", std::span<const std::string>{}, arena.cpp_root, "envdump");
  INFO("env dump:\n" << got.out);
  REQUIRE(got.code == 0);
  auto const agent = agent_db_from_env_dump(got.out);
  REQUIRE(agent.has_value());
  REQUIRE(*agent == (arena.cpp_root / "agent.db").string());
  // The main database pin is unchanged and sits beside it.
  REQUIRE(got.out.contains(std::format("PLANAR_DB={}\n", (arena.cpp_root / "planar.db").string())));
}

TEST_CASE("arena: launch_pinned_detached exports the same PLANAR_AGENT_DB as run_pinned", "[arena][agentdb]") {
  auto const arena = make_arena("agentdb_detached");
  launch_pinned_detached("/usr/bin/env", std::span<const std::string>{}, arena.cpp_root, "envdump");
  // `env` exits on its own the moment it has printed, so there is nothing
  // to reap; the sentinel is its stdout capture.
  auto const seen_at = await_sentinel(arena.cpp_root / "envdump.out", true, std::chrono::seconds(10));
  REQUIRE(seen_at.has_value());
  // The file is complete once `env` has exited; poll briefly for the last
  // line rather than trusting the first byte.
  std::string dump;
  for (int i = 0; i < 200; ++i) {
    dump = read_all(arena.cpp_root / "envdump.out");
    if (agent_db_from_env_dump(dump).has_value() && dump.ends_with('\n')) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  INFO("env dump:\n" << dump);
  auto const agent = agent_db_from_env_dump(dump);
  REQUIRE(agent.has_value());
  REQUIRE(*agent == (arena.cpp_root / "agent.db").string());
}

// ---------------------------------------------------------------------------
// Scenario: Edge — the scripts and the smoke recipe pin the agent database
// ---------------------------------------------------------------------------

TEST_CASE("scripts: exit-code-contract.sh pins PLANAR_AGENT_DB beside PLANAR_DB", "[arena][agentdb][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("agentdb_exitcode");
  auto const record = arena.cpp_root / "record.tsv";
  auto const bin    = arena.cpp_root / "bin";
  for (auto const* name : {"planar", "planar-agent", "planar-watch", "planar-ext"}) {
    write_wrapper(bin, name, record);
  }
  // The gate compares exit codes against docs/cli-reference.md and will
  // report mismatches against a wrapper that always exits 0; its own status
  // is not under test here, only the environment it hands the binaries.
  static_cast<void>(shell(std::format("cd {} && scripts/exit-code-contract.sh --bin-dir {} >/dev/null 2>&1",
                                      shell_quote(root.string()), shell_quote(bin.string()))));
  require_pinned_beside_db("scripts/exit-code-contract.sh", rows(record));
}

TEST_CASE("scripts: coverage-check.sh pins PLANAR_AGENT_DB beside PLANAR_DB", "[arena][agentdb][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("agentdb_coverage");
  auto const record = arena.cpp_root / "record.tsv";
  auto const bin    = arena.cpp_root / "bin";
  write_wrapper(bin, "planar", record);
  static_cast<void>(shell(std::format("cd {} && PLANAR_BIN={} scripts/coverage-check.sh --report >/dev/null 2>&1",
                                      shell_quote(root.string()), shell_quote((bin / "planar").string()))));
  require_pinned_beside_db("scripts/coverage-check.sh", rows(record));
}

TEST_CASE("scripts: surface-snapshot.sh pins PLANAR_AGENT_DB beside PLANAR_DB", "[arena][agentdb][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("agentdb_surface");
  auto const record = arena.cpp_root / "record.tsv";
  // The script has no binary-path flag: it resolves `<its own dir>/../
  // build/debug/bin`. So it is run from a COPY placed in a scratch tree
  // whose `build/debug/bin` holds the wrappers; the script itself is byte
  // identical to the one in the checkout.
  auto const fake_root = arena.cpp_root / "repo";
  std::filesystem::create_directories(fake_root / "scripts");
  std::filesystem::copy_file(root / "scripts" / "surface-snapshot.sh", fake_root / "scripts" / "surface-snapshot.sh");
  for (auto const* name : {"planar", "planar-agent", "planar-watch", "planar-ext"}) {
    write_wrapper(fake_root / "build" / "debug" / "bin", name, record);
  }
  // `capture` writes the baseline into the scratch copy; `verify` would
  // only add a drift failure against a baseline that does not exist there.
  static_cast<void>(
      shell(std::format("cd {} && scripts/surface-snapshot.sh capture >/dev/null 2>&1", shell_quote(fake_root.string()))));
  require_pinned_beside_db("scripts/surface-snapshot.sh", rows(record));
}

TEST_CASE("make smoke: the recipe pins PLANAR_AGENT_DB beside the throwaway PLANAR_DB", "[arena][agentdb][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("agentdb_smoke");
  auto const record = arena.cpp_root / "record.tsv";
  auto const bin    = arena.cpp_root / "bin";
  write_wrapper(bin, "planar", record);
  // The recipe's first step is `cmake --build`; a no-op `cmake` first on
  // PATH keeps this from touching (or racing) the real build directory, and
  // `CPP_BUILD_DIR` under the arena keeps the throwaway database there too.
  auto const fake_path = arena.cpp_root / "path";
  std::filesystem::create_directories(fake_path);
  {
    std::ofstream out(fake_path / "cmake", std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\nexit 0\n";
  }
  std::filesystem::permissions(fake_path / "cmake", std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                                        std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                                        std::filesystem::perms::others_exec);
  auto const build_dir = arena.cpp_root / "build";
  auto const status    = shell(std::format("cd {} && PATH={}:\"$PATH\" make smoke CPP_BIN_DIR={} CPP_BUILD_DIR={} "
                                           "ARGS=version >/dev/null 2>&1",
                                           shell_quote(root.string()), shell_quote(fake_path.string()), shell_quote(bin.string()),
                                           shell_quote(build_dir.string())));
  INFO("make smoke status: " << status);
  auto const seen_rows = rows(record);
  require_pinned_beside_db("make smoke", seen_rows);
  REQUIRE(seen_rows.size() == 1);
  REQUIRE(seen_rows.front().agent_db == (build_dir / ".smoke" / "agent.db").string());
  REQUIRE(seen_rows.front().db == (build_dir / ".smoke" / "planar.db").string());
}

// ---------------------------------------------------------------------------
// Scenario: Error — an agent database path outside the arena fails the case
// ---------------------------------------------------------------------------

namespace {

using planar::cmd::parity::agent_db_pin_error;
using planar::cmd::parity::pinned_env;
using planar::cmd::parity::pinned_var;

/// @brief A pinned map with `PLANAR_AGENT_DB` replaced (or, with an empty
/// `value`, removed) and `HOME` optionally replaced.
/// @param work The arena root.
/// @param agent_db The `PLANAR_AGENT_DB` value, or empty to drop the variable.
/// @param home The `HOME` value, or empty to leave the default.
/// @return The map.
auto env_with(const std::filesystem::path& work, std::string agent_db, std::string home = {}) -> std::vector<pinned_var> {
  std::vector<pinned_var> out;
  for (auto& var : pinned_env(work)) {
    if (var.name == "PLANAR_AGENT_DB") {
      if (!agent_db.empty()) {
        out.push_back(pinned_var{.name = var.name, .value = agent_db});
      }
      continue;
    }
    if (var.name == "HOME" && !home.empty()) {
      out.push_back(pinned_var{.name = var.name, .value = home});
      continue;
    }
    out.push_back(std::move(var));
  }
  return out;
}

} // namespace

TEST_CASE("arena: agent_db_pin_error accepts the default map and refuses one that resolves outside", "[arena][agentdb]") {
  auto const arena   = make_arena("agentdb_predicate");
  auto const work    = arena.cpp_root;
  auto const outside = (std::filesystem::temp_directory_path() / "planar_agentdb_elsewhere" / "agent.db").string();

  SECTION("the default map is pinned") {
    auto const env = pinned_env(work);
    REQUIRE_FALSE(agent_db_pin_error(work, env).has_value());
  }
  SECTION("PLANAR_AGENT_DB outside the arena is named in the diagnostic") {
    auto const problem = agent_db_pin_error(work, env_with(work, outside));
    REQUIRE(problem.has_value());
    REQUIRE_THAT(*problem, Catch::Matchers::ContainsSubstring(outside));
    REQUIRE_THAT(*problem, Catch::Matchers::ContainsSubstring(work.string()));
    REQUIRE_THAT(*problem, Catch::Matchers::ContainsSubstring("PLANAR_AGENT_DB"));
  }
  SECTION("a relative PLANAR_AGENT_DB is refused: it depends on the cwd, not the arena") {
    auto const problem = agent_db_pin_error(work, env_with(work, "agent.db"));
    REQUIRE(problem.has_value());
  }
  SECTION("a path that only looks nested (`..` after the root) is refused") {
    auto const problem = agent_db_pin_error(work, env_with(work, (work / ".." / "agent.db").string()));
    REQUIRE(problem.has_value());
  }
  SECTION("with PLANAR_AGENT_DB absent the HOME fallback is resolved, and a scratch HOME contains it") {
    auto const env = env_with(work, "");
    REQUIRE_FALSE(agent_db_pin_error(work, env).has_value());
  }
  SECTION("with PLANAR_AGENT_DB absent and HOME outside, the fallback is refused and named") {
    auto const home    = (std::filesystem::temp_directory_path() / "planar_agentdb_elsewhere_home").string();
    auto const problem = agent_db_pin_error(work, env_with(work, "", home));
    REQUIRE(problem.has_value());
    REQUIRE_THAT(*problem, Catch::Matchers::ContainsSubstring(home));
    REQUIRE_THAT(*problem, Catch::Matchers::ContainsSubstring("fallback"));
  }
  SECTION("with neither variable the map is unpinned and refused") {
    std::vector<pinned_var> env;
    for (auto& var : pinned_env(work)) {
      if (var.name != "PLANAR_AGENT_DB" && var.name != "HOME") {
        env.push_back(std::move(var));
      }
    }
    auto const problem = agent_db_pin_error(work, env);
    REQUIRE(problem.has_value());
    REQUIRE_THAT(*problem, Catch::Matchers::ContainsSubstring("unpinned"));
  }
}

TEST_CASE("arena: run_pinned fails the case before the binary runs when the agent database is outside", "[arena][agentdb]") {
  auto const arena   = make_arena("agentdb_refuse_run");
  auto const record  = arena.cpp_root / "record.tsv";
  auto const bin     = arena.cpp_root / "bin";
  auto const outside = (std::filesystem::temp_directory_path() / "planar_agentdb_elsewhere" / "agent.db").string();
  write_wrapper(bin, "planar", record);
  REQUIRE_THROWS_WITH(
      run_pinned(bin / "planar", std::span<const std::string>{}, arena.cpp_root, "refused", env_with(arena.cpp_root, outside)),
      Catch::Matchers::ContainsSubstring("outside the arena root"));
  // The wrapper never ran: no row was recorded and no capture file exists.
  REQUIRE(rows(record).empty());
  REQUIRE_FALSE(std::filesystem::exists(arena.cpp_root / "refused.out"));
}

TEST_CASE("arena: launch_pinned_detached fails the case before the launch when the agent database is outside",
          "[arena][agentdb]") {
  auto const arena   = make_arena("agentdb_refuse_launch");
  auto const record  = arena.cpp_root / "record.tsv";
  auto const bin     = arena.cpp_root / "bin";
  auto const outside = (std::filesystem::temp_directory_path() / "planar_agentdb_elsewhere" / "agent.db").string();
  write_wrapper(bin, "planar", record);
  REQUIRE_THROWS_WITH(launch_pinned_detached(bin / "planar", std::span<const std::string>{}, arena.cpp_root, "refused",
                                             env_with(arena.cpp_root, outside)),
                      Catch::Matchers::ContainsSubstring("outside the arena root"));
  REQUIRE(rows(record).empty());
  REQUIRE_FALSE(std::filesystem::exists(arena.cpp_root / "refused.out"));
}
