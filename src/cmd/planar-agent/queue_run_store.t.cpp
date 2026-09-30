// queue_run_store.t.cpp: `planar-agent queue run` when the agent database
// cannot be reached, as an operator sees it (plan 1080, task
// hq-store-unreachable and its folded caveat hq-store-unreachable-maindb;
// tech spec 647 § The queue cannot be reached; decision 1188).
//
// The rule under test: a store that cannot be opened or written, or that
// cannot be located at all, REFUSES the command. `queue run` prints one line
// naming the reason on stderr, exits 125, and never runs the command
// directly; and it reaches that verdict without the MAIN database, which it
// does not use, having any say.
//
// Every case runs the BUILT binary through `run_pinned` in an arena with its
// own `PLANAR_AGENT_DB`, `HOME` and `PLANAR_CONFIG_PATH`. The cases that take
// variables away ask the harness to REMOVE them from the child's environment
// (`pinned_var::unset`), and the harness accepts only the map in which
// `HOME` and `PLANAR_AGENT_DB` are both removed, because the runtime then has
// nothing to resolve a store path from; a map that merely omits them would
// inherit the operator's own.
//
// The command a case queues is a script in the arena's `fakebin`, first on
// `PATH`, that creates a marker file. "The command was never run" is a file
// that does not exist, not an inference from an exit code.

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.agentdb;
import planar.engine.hostqueue;

#include "parity_harness.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
namespace parity = planar::cmd::parity;

using parity::capture;
using parity::pinned_var;
using parity::read_all;

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

auto present(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

auto marker(const parity::arena& arena) -> std::filesystem::path {
  return arena.cpp_root / "ran_probe";
}

/// @brief Writes `fakebin/probe`, which creates the marker and exits with 7,
/// so a run is visible both as a file and as a status no refusal returns.
void make_probe(const parity::arena& arena) {
  auto const dir = arena.cpp_root / "fakebin";
  std::filesystem::create_directories(dir);
  auto const path = dir / "probe";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n: > " << parity::shell_quote(marker(arena).string()) << "\nexit 7\n";
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

/// @brief The default pinned map with `fakebin` in front of `PATH`.
auto base_env(const parity::arena& arena) -> std::vector<pinned_var> {
  auto       env       = parity::pinned_env(arena.cpp_root);
  auto const inherited = std::getenv("PATH");
  env.push_back(pinned_var{
      .name  = "PATH",
      .value = std::format("{}:{}", (arena.cpp_root / "fakebin").string(), inherited != nullptr ? inherited : "/usr/bin:/bin")});
  return env;
}

/// @brief Replaces `name`'s value in `env`.
void set_var(std::vector<pinned_var>& env, std::string_view name, std::string value) {
  for (auto& var : env) {
    if (var.name == name) {
      var.value = std::move(value);
      var.unset = false;
      return;
    }
  }
  env.push_back(pinned_var{.name = std::string{name}, .value = std::move(value)});
}

/// @brief Removes `name` from the child's environment.
void remove_var(std::vector<pinned_var>& env, std::string_view name) {
  for (auto& var : env) {
    if (var.name == name) {
      var.unset = true;
      return;
    }
  }
  env.push_back(pinned_var{.name = std::string{name}, .unset = true});
}

auto run_probe(const parity::arena& arena, std::string_view tag, const std::vector<pinned_var>& env) -> capture {
  std::vector<std::string> const args{"queue", "run", "--", "probe"};
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

/// @brief The refusal contract: 125, nothing on stdout, exactly one stderr
/// line naming the reason, and the command never started.
void require_refused(const parity::arena& arena, const capture& got, std::string_view reason) {
  INFO("stdout:\n" << got.out << "stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err.starts_with("error: queue: "));
  CHECK(std::ranges::count(got.err, '\n') == 1);
  CHECK(got.err.ends_with('\n'));
  CHECK(got.err.find(reason) != std::string::npos);
  CHECK_FALSE(present(marker(arena)));
}

/// @brief Restores a directory's mode on the way out, so the arena's own
/// clean-up can remove it even when an assertion fails.
struct mode_guard {
  std::filesystem::path path;
  ~mode_guard() {
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
  }
};

/// @brief Whether permission bits can make a path unwritable here. Root
/// ignores them, so the two cases that rely on them assert nothing there and
/// the rest of this file carries the contract.
auto permissions_bind() -> bool {
  return ::geteuid() != 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Error: the agent database cannot be opened or written
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a store in a directory that cannot be written refuses at 125 and runs nothing",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_rodir");
  make_probe(arena);
  auto const dir = arena.cpp_root / "readonly";
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  mode_guard const restore{dir};
  if (!permissions_bind()) {
    return;
  }

  auto env = base_env(arena);
  set_var(env, "PLANAR_AGENT_DB", (dir / "agent.db").string());
  auto const got = run_probe(arena, "rodir", env);
  require_refused(arena, got, (dir / "agent.db").string());
  CHECK_FALSE(present(dir / "agent.db"));
}

TEST_CASE("queue run: a store file that cannot be written refuses at 125 and runs nothing", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_rofile");
  make_probe(arena);
  auto const store = arena.cpp_root / "readonly.db";
  {
    std::ofstream out(store, std::ios::binary | std::ios::trunc);
  }
  std::filesystem::permissions(store, std::filesystem::perms::owner_read);
  mode_guard const restore{store};
  if (!permissions_bind()) {
    return;
  }

  auto env = base_env(arena);
  set_var(env, "PLANAR_AGENT_DB", store.string());
  auto const got = run_probe(arena, "rofile", env);
  require_refused(arena, got, store.string());
  CHECK(std::filesystem::file_size(store) == 0);
}

TEST_CASE("queue run: a store path that is a directory refuses at 125 and runs nothing", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_isdir");
  make_probe(arena);
  auto const store = arena.cpp_root / "store-is-a-directory";
  std::filesystem::create_directories(store);

  auto env = base_env(arena);
  set_var(env, "PLANAR_AGENT_DB", store.string());
  auto const got = run_probe(arena, "isdir", env);
  require_refused(arena, got, store.string());
  CHECK(std::filesystem::is_directory(store));
}

TEST_CASE("queue run: a store whose parent is a regular file refuses at 125 and runs nothing",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_parentfile");
  make_probe(arena);
  auto const blocker = arena.cpp_root / "blocker";
  {
    std::ofstream out(blocker, std::ios::binary | std::ios::trunc);
    out << "not a directory";
  }

  auto env = base_env(arena);
  set_var(env, "PLANAR_AGENT_DB", (blocker / "agent.db").string());
  auto const got = run_probe(arena, "parentfile", env);
  require_refused(arena, got, "blocker");
  CHECK(read_all(blocker) == "not a directory");
}

TEST_CASE("queue run: a file that is not a database refuses at 125 and is left as it was", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_garbage");
  make_probe(arena);
  auto const store = arena.cpp_root / "garbage.db";
  auto const bytes = std::string(4096, 'x') + "this is not an SQLite database";
  {
    std::ofstream out(store, std::ios::binary | std::ios::trunc);
    out << bytes;
  }

  auto env = base_env(arena);
  set_var(env, "PLANAR_AGENT_DB", store.string());
  auto const got = run_probe(arena, "garbage", env);
  require_refused(arena, got, store.string());
  CHECK(read_all(store) == bytes);
}

// ---------------------------------------------------------------------------
// Scenario: Error: no agent database location can be found
// ---------------------------------------------------------------------------

TEST_CASE("queue run: neither HOME nor PLANAR_AGENT_DB refuses at 125 with or without a main database path",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_noloc");
  make_probe(arena);

  SECTION("PLANAR_DB is also unset") {
    auto env = base_env(arena);
    remove_var(env, "HOME");
    remove_var(env, "PLANAR_AGENT_DB");
    remove_var(env, "PLANAR_DB");
    auto const got = run_probe(arena, "noloc_nodb", env);
    require_refused(arena, got, "PLANAR_AGENT_DB");
    CHECK(got.err.find("HOME") != std::string::npos);
  }
  SECTION("PLANAR_DB is set") {
    auto env = base_env(arena);
    remove_var(env, "HOME");
    remove_var(env, "PLANAR_AGENT_DB");
    auto const got = run_probe(arena, "noloc_db", env);
    require_refused(arena, got, "PLANAR_AGENT_DB");
    CHECK(got.err.find("HOME") != std::string::npos);
  }
  // Nothing was created anywhere the arena owns, either.
  CHECK_FALSE(present(arena.cpp_root / "agent.db"));
  CHECK_FALSE(present(arena.cpp_root / "planar.db"));
}

// ---------------------------------------------------------------------------
// Scenario: Edge: Planar's main database is unavailable (7059)
// ---------------------------------------------------------------------------

TEST_CASE("queue run: runs when only PLANAR_AGENT_DB is set and HOME and PLANAR_DB are not", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_agentonly");
  make_probe(arena);

  auto env = base_env(arena);
  remove_var(env, "HOME");
  remove_var(env, "PLANAR_DB");
  auto const got = run_probe(arena, "agentonly", env);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7); // The command's own status, passed through.
  CHECK(got.err.empty());
  CHECK(present(marker(arena)));
  CHECK_FALSE(present(arena.cpp_root / "planar.db"));

  auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
  REQUIRE(opened.has_value());
  auto history = hq::list_history(*opened);
  REQUIRE(history.has_value());
  REQUIRE(history->size() == 1);
  CHECK(history->front().outcome == hq::history_outcome::exited);
  CHECK(history->front().exit_code == 7);
}

TEST_CASE("queue run: an unusable main database path does not stop it, but stops every other verb as before",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_mainpath");
  make_probe(arena);
  auto env = base_env(arena);
  remove_var(env, "HOME");
  remove_var(env, "PLANAR_DB");

  // The other verbs keep their exit code and their words: an unresolvable main
  // database is refused before dispatch, even for a verb that never opens it.
  for (auto const& verb : std::vector<std::vector<std::string>>{{"version"}, {"schema"}, {"pull", "1"}}) {
    INFO(verb.front());
    auto const got = parity::run_pinned(agent_bin(), verb, arena.cpp_root, "mainpath_other", env);
    CHECK(got.code == 1);
    CHECK(got.out.empty());
    CHECK(got.err.find("neither PLANAR_DB nor HOME is set; cannot locate the Planar database") != std::string::npos);
  }

  // The queue verb, in the same environment, is not.
  auto const got = run_probe(arena, "mainpath_queue", env);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7);
  CHECK(present(marker(arena)));
}
