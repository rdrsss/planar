// @file arena_pins.t.cpp
// @brief The arena pins every entry point that runs a from-source binary to
// scratch state: the harness strips an inherited `PLANAR_QUEUE_SLOT`, and the
// three gate scripts and the `make smoke` recipe pin `PLANAR_DB`.
//
// This file replaces `arena_agent_db.t.cpp` (plan 1089 M1). The agent
// database is gone, so nothing pins `PLANAR_AGENT_DB` any more; the pins that
// outlive it are asserted here. Each script case runs the script with a
// RECORDING WRAPPER in place of the binary and asserts on the environment the
// wrapper saw, so nothing here runs a real `planar` and nothing can reach
// `~/.planar` even when an assertion is wrong. The scripts run with
// `PLANAR_AGENT_DB` removed from their environment and must not set it:
// decision 1229 makes the variable ignored, never pinned or refused.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdlib.h>

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

/// @brief The rule every recorded invocation must satisfy: `PLANAR_DB` is
/// pinned, absolute, and nowhere under the real `~/.planar`; and the script
/// did not set `PLANAR_AGENT_DB` (the test removed it from the environment
/// the script started with, and the agent database no longer exists).
/// @param label Which entry point produced the rows.
/// @param seen_rows The rows.
void require_db_pinned(std::string_view label, const std::vector<seen>& seen_rows) {
  INFO("entry point: " << label);
  REQUIRE_FALSE(seen_rows.empty());
  char const* home      = std::getenv("HOME");
  auto const  real_home = std::filesystem::path{home == nullptr ? "" : home} / ".planar";
  for (auto const& row : seen_rows) {
    INFO("invocation of " << row.binary << ": PLANAR_DB=" << row.db << " PLANAR_AGENT_DB=" << row.agent_db);
    REQUIRE(row.db != "<unset>");
    REQUIRE(std::filesystem::path{row.db}.is_absolute());
    REQUIRE_FALSE(row.db.starts_with(real_home.string()));
    CHECK(row.agent_db == "<unset>");
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Happy path — the arena isolates with PLANAR_DB and HOME alone
// ---------------------------------------------------------------------------

TEST_CASE("arena: PLANAR_DB and HOME resolve inside the arena, no PLANAR_AGENT_DB is pinned, and a queue run leaves a HOME "
          "canary alone",
          "[arena][queue]") {
  auto const arena = make_arena("pins_canary");

  // The map pins the database and HOME under the arena root and names no
  // agent database (decision 1229: PLANAR_AGENT_DB is ignored, not pinned).
  std::optional<std::string> db;
  std::optional<std::string> home;
  for (auto const& var : planar::cmd::parity::pinned_env(arena.cpp_root)) {
    CHECK(var.name != "PLANAR_AGENT_DB");
    if (var.name == "PLANAR_DB") {
      db = var.value;
    } else if (var.name == "HOME") {
      home = var.value;
    }
  }
  REQUIRE(db.has_value());
  REQUIRE(home.has_value());
  CHECK(*db == (arena.cpp_root / "planar.db").string());
  CHECK(std::filesystem::path{*home}.parent_path() == arena.cpp_root);

  // A canary under the scratch HOME/.planar, where the old agent store's
  // fallback path lived. It must be byte-for-byte unchanged afterwards.
  auto const canary_dir = std::filesystem::path{*home} / ".planar";
  std::filesystem::create_directories(canary_dir);
  auto const canary = canary_dir / "canary.txt";
  {
    std::ofstream out(canary, std::ios::binary | std::ios::trunc);
    out << "canary\n";
  }

  auto const initialised =
      run_pinned(std::filesystem::path{PLANAR_CPP_BIN}, std::vector<std::string>{"init", "--skip-project", "--allow-no-repo"},
                 arena.cpp_root, "init");
  INFO("init stderr:\n" << initialised.err);
  REQUIRE(initialised.code == 0);
  auto const queued = run_pinned(std::filesystem::path{PLANAR_AGENT_CPP_BIN},
                                 std::vector<std::string>{"queue", "run", "--", "/usr/bin/true"}, arena.cpp_root, "queue_true");
  INFO("queue run stderr:\n" << queued.err);
  REQUIRE(queued.code == 0);

  CHECK(read_all(canary) == "canary\n");
  CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "agent.db"));
  CHECK_FALSE(std::filesystem::exists(canary_dir / "agent.db"));
  CHECK(std::filesystem::exists(arena.cpp_root / "planar.db"));
}

// ---------------------------------------------------------------------------
// Scenario: Edge — the arena strips an inherited slot marker (task 7061)
// ---------------------------------------------------------------------------

namespace {

/// @brief Sets an environment variable for one scope and puts the previous
/// state back, so a case that plants `PLANAR_QUEUE_SLOT` in this process (as a
/// suite run under `queue run -- make test` inherits it) leaves nothing behind.
struct scoped_env {
  std::string                name;
  std::optional<std::string> previous;

  scoped_env(std::string variable, const std::string& value) : name(std::move(variable)) {
    if (auto const* old = std::getenv(name.c_str()); old != nullptr) {
      previous = old;
    }
    ::setenv(name.c_str(), value.c_str(), 1);
  }
  scoped_env(const scoped_env&)            = delete;
  scoped_env& operator=(const scoped_env&) = delete;
  ~scoped_env() {
    if (previous) {
      ::setenv(name.c_str(), previous->c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }
};

/// @brief The value `PLANAR_QUEUE_SLOT` has in an `env` dump, when the line
/// is there.
auto slot_from_env_dump(const std::string& dump) -> std::optional<std::string> {
  std::istringstream in(dump);
  std::string        line;
  while (std::getline(in, line)) {
    if (line.starts_with("PLANAR_QUEUE_SLOT=")) {
      return line.substr(std::string_view{"PLANAR_QUEUE_SLOT="}.size());
    }
  }
  return std::nullopt;
}

/// @brief The value `name` has in an `env` dump, when the line is there.
auto var_from_env_dump(const std::string& dump, std::string_view name) -> std::optional<std::string> {
  auto const         prefix = std::format("{}=", name);
  std::istringstream in(dump);
  std::string        line;
  while (std::getline(in, line)) {
    if (line.starts_with(prefix)) {
      return line.substr(prefix.size());
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("arena: run_pinned and launch_pinned_detached strip an inherited PLANAR_QUEUE_SLOT", "[arena][queue]") {
  // A suite run under `planar-agent queue run -- make test` inherits the
  // marker, and every arena submitter would then look nested.
  scoped_env const inherited{"PLANAR_QUEUE_SLOT", "424242"};
  REQUIRE(std::getenv("PLANAR_QUEUE_SLOT") != nullptr); // the plant took effect
  // A PLANAR_DB inherited from this process must not be able to stand in for
  // the arena's own: plant a foreign one so only an arena-pinned value passes.
  scoped_env const foreign_db{"PLANAR_DB", "/nonexistent/foreign/planar.db"};

  auto const arena    = make_arena("slot_run");
  auto const arena_db = (arena.cpp_root / "planar.db").string();
  auto const got      = run_pinned("/usr/bin/env", std::span<const std::string>{}, arena.cpp_root, "envdump");
  INFO("env dump:\n" << got.out);
  REQUIRE(got.code == 0);
  REQUIRE(var_from_env_dump(got.out, "PLANAR_DB") == std::optional<std::string>{arena_db}); // a real dump, arena-pinned
  CHECK_FALSE(slot_from_env_dump(got.out).has_value());

  launch_pinned_detached("/usr/bin/env", std::span<const std::string>{}, arena.cpp_root, "envdump_detached");
  REQUIRE(await_sentinel(arena.cpp_root / "envdump_detached.out", true, std::chrono::seconds(10)).has_value());
  std::string dump;
  for (int i = 0; i < 200; ++i) {
    dump = read_all(arena.cpp_root / "envdump_detached.out");
    if (var_from_env_dump(dump, "PLANAR_DB").has_value() && dump.ends_with('\n')) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  INFO("detached env dump:\n" << dump);
  REQUIRE(var_from_env_dump(dump, "PLANAR_DB") == std::optional<std::string>{arena_db});
  CHECK_FALSE(slot_from_env_dump(dump).has_value());
}

TEST_CASE("arena: a map that names PLANAR_QUEUE_SLOT still delivers it", "[arena][queue]") {
  // Stripping is the default, not a ban: a case that plays a queued command
  // hands the marker over explicitly, and that value must arrive.
  scoped_env const inherited{"PLANAR_QUEUE_SLOT", "424242"};
  auto const       arena = make_arena("slot_explicit");
  auto             vars  = planar::cmd::parity::pinned_env(arena.cpp_root);
  vars.push_back(planar::cmd::parity::pinned_var{.name = "PLANAR_QUEUE_SLOT", .value = "5"});
  auto const got = run_pinned("/usr/bin/env", std::span<const std::string>{}, arena.cpp_root, "envdump", vars);
  INFO("env dump:\n" << got.out);
  REQUIRE(got.code == 0);
  CHECK(slot_from_env_dump(got.out) == std::optional<std::string>{"5"});
}

// ---------------------------------------------------------------------------
// Scenario: Edge — the scripts and the smoke recipe pin PLANAR_DB, not agent.db
// ---------------------------------------------------------------------------

TEST_CASE("scripts: exit-code-contract.sh pins PLANAR_DB and sets no PLANAR_AGENT_DB", "[arena][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("pins_exitcode");
  auto const record = arena.cpp_root / "record.tsv";
  auto const bin    = arena.cpp_root / "bin";
  for (auto const* name : {"planar", "planar-agent", "planar-watch", "planar-ext"}) {
    write_wrapper(bin, name, record);
  }
  // The gate compares exit codes against docs/cli-reference.md and will
  // report mismatches against a wrapper that always exits 0; its own status
  // is not under test here, only the environment it hands the binaries.
  static_cast<void>(
      shell(std::format("cd {} && env -u PLANAR_AGENT_DB scripts/exit-code-contract.sh --bin-dir {} >/dev/null 2>&1",
                        shell_quote(root.string()), shell_quote(bin.string()))));
  require_db_pinned("scripts/exit-code-contract.sh", rows(record));
}

TEST_CASE("scripts: coverage-check.sh pins PLANAR_DB and sets no PLANAR_AGENT_DB", "[arena][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("pins_coverage");
  auto const record = arena.cpp_root / "record.tsv";
  auto const bin    = arena.cpp_root / "bin";
  write_wrapper(bin, "planar", record);
  static_cast<void>(
      shell(std::format("cd {} && env -u PLANAR_AGENT_DB PLANAR_BIN={} scripts/coverage-check.sh --report >/dev/null 2>&1",
                        shell_quote(root.string()), shell_quote((bin / "planar").string()))));
  require_db_pinned("scripts/coverage-check.sh", rows(record));
}

TEST_CASE("scripts: surface-snapshot.sh pins PLANAR_DB and sets no PLANAR_AGENT_DB", "[arena][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("pins_surface");
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
  static_cast<void>(shell(std::format("cd {} && env -u PLANAR_AGENT_DB scripts/surface-snapshot.sh capture >/dev/null 2>&1",
                                      shell_quote(fake_root.string()))));
  require_db_pinned("scripts/surface-snapshot.sh", rows(record));
}

TEST_CASE("make smoke: the recipe pins the throwaway PLANAR_DB and sets no PLANAR_AGENT_DB", "[arena][scripts]") {
  auto const root   = repo_root();
  auto const arena  = make_arena("pins_smoke");
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
  auto const status    = shell(std::format(
      "cd {} && env -u PLANAR_AGENT_DB PATH={}:\"$PATH\" make smoke CPP_BIN_DIR={} CPP_BUILD_DIR={} "
      "ARGS=version >/dev/null 2>&1",
      shell_quote(root.string()), shell_quote(fake_path.string()), shell_quote(bin.string()), shell_quote(build_dir.string())));
  INFO("make smoke status: " << status);
  auto const seen_rows = rows(record);
  require_db_pinned("make smoke", seen_rows);
  REQUIRE(seen_rows.size() == 1);
  REQUIRE(seen_rows.front().db == (build_dir / ".smoke" / "planar.db").string());
}
