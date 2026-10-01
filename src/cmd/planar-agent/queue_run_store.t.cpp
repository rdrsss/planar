// queue_run_store.t.cpp: `planar-agent queue run`, `queue cancel` and `queue
// status` and the database they open, as an operator sees it (plan 1089, task
// qp-agent-queue-open and qp-agent-queue-tests; tech spec 656 § Store and
// open path; decisions 1219-1221; this file began as plan 1080's
// hq-store-unreachable cases, tech spec 647 § The queue cannot be reached).
//
// The rule under test: the queue's state is in `planar.db`. A database that
// cannot be opened, cannot be located, is behind, or whose queue tables this
// binary cannot use REFUSES the command: one `error: queue:` line on stderr,
// exit 125, the command never run, and `planar.db` never created. An ahead
// database whose queue marker admits this binary is used as it is.
//
// Every case runs the BUILT binary through `run_pinned` in an arena with its
// own `PLANAR_DB`, `HOME` and `PLANAR_CONFIG_PATH`; the database fixtures
// come from queue_test_store.hpp. The cases that take variables away ask the
// harness to REMOVE them from the child's environment (`pinned_var::unset`).
//
// The command a case queues is a script in the arena's `fakebin`, first on
// `PATH`, that creates a marker file. "The command was never run" is a file
// that does not exist, not an inference from an exit code.

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.hostqueue;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

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
/// ignores them, so the two cases that rely on them assert nothing there; the
/// symbolic-link-loop cases below reach the same refusals by a path root
/// respects, and the two permission cases say so visibly when they stand down
/// (task 7073).
auto permissions_bind() -> bool {
  return ::geteuid() != 0;
}

/// @brief The visible note the two permission-bit cases leave when they stand
/// down for root, so a green run as root is not read as proof that an
/// unwritable directory or file was exercised.
void note_permissions_do_not_bind() {
  WARN("running as root: permission bits do not bind, so this case asserted nothing; the symbolic-link-loop cases cover the "
       "same refusals for this user");
}

/// @brief Sets an environment variable for one scope and puts the previous
/// state back, so a failed REQUIRE cannot leak the variable into later cases.
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

} // namespace

// ---------------------------------------------------------------------------
// The harness's removals reach the child
// ---------------------------------------------------------------------------

TEST_CASE("queue run store cases: a variable the map removes is absent from the child's environment",
          "[cmd][agent][queue][queue-store]") {
  // Every no-HOME case above rests on `env -u` really removing the variable;
  // if the prefix dropped it, the child would inherit the operator's HOME and
  // the run would use (and migrate) stores outside the arena. The child here
  // is `env` itself, printing what it received.
  auto const arena = parity::make_arena("qs_removal");
  // Planted in this process's own environment (restored on every exit path,
  // including a failed REQUIRE), so neither check depends on what the runner
  // happens to have exported.
  auto const       planted_db = (arena.cpp_root / "planted.db").string();
  scoped_env const probe{"PLANAR_PROBE_INHERITED", "yes"};
  scoped_env const planar_db{"PLANAR_DB", planted_db};

  auto env = parity::pinned_env(arena.cpp_root);
  set_var(env, "PLANAR_DB", planted_db);
  auto const printed = [&](std::string_view tag) {
    return parity::run_pinned("/usr/bin/env", std::span<const std::string>{}, arena.cpp_root, tag, env);
  };
  auto const has_line = [](const std::string& text, std::string_view line) {
    return ("\n" + text).find(std::format("\n{}", line)) != std::string::npos;
  };

  // Control: an unlisted variable is inherited, so the removal below is what
  // makes it disappear.
  auto const control = printed("qs_removal_control");
  REQUIRE(control.code == 0);
  CHECK(has_line(control.out, "PLANAR_PROBE_INHERITED=yes"));
  CHECK(has_line(control.out, std::format("PLANAR_DB={}", planted_db)));

  remove_var(env, "PLANAR_PROBE_INHERITED");
  remove_var(env, "PLANAR_DB");
  remove_var(env, "HOME");
  auto const got = printed("qs_removal_run");
  INFO("child environment:\n" << got.out);
  REQUIRE(got.code == 0);
  CHECK_FALSE(has_line(got.out, "PLANAR_PROBE_INHERITED="));
  CHECK_FALSE(has_line(got.out, "PLANAR_DB="));
  CHECK_FALSE(has_line(got.out, "HOME="));
}

// ---------------------------------------------------------------------------
// Helpers for the planar.db cases
// ---------------------------------------------------------------------------

namespace {

namespace qfix = planar::cmd::qfix;

/// @brief The arena's `PLANAR_DB`.
auto main_db(const parity::arena& arena) -> std::filesystem::path {
  return arena.cpp_root / "planar.db";
}

/// @brief The default pinned map with `PLANAR_DB` replaced by `db`.
auto env_for(const parity::arena& arena, const std::filesystem::path& db) -> std::vector<pinned_var> {
  auto env = base_env(arena);
  set_var(env, "PLANAR_DB", db.string());
  return env;
}

auto run_verb(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& args,
              const std::vector<pinned_var>& env) -> capture {
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

/// @brief `queue status 1 --json` against `env`.
auto status_json(const parity::arena& arena, std::string_view tag, const std::vector<pinned_var>& env) -> capture {
  return run_verb(arena, tag, {"queue", "status", "1", "--json"}, env);
}

auto has_tag(const capture& got, std::string_view tag) -> bool {
  return got.out.find(std::format("\"tag\":\"{}\"", tag)) != std::string::npos;
}

/// @brief The seam-free ticket of `queue run --detach`: the sequence number
/// and the log path.
struct issued {
  std::int64_t seq = 0;
  std::string  path;
};

auto read_ticket(const capture& got) -> issued {
  INFO("stdout:\n" << got.out << "stderr:\n" << got.err);
  REQUIRE(got.code == 0);
  auto const first = got.out.find('\n');
  REQUIRE(first != std::string::npos);
  auto const second = got.out.find('\n', first + 1);
  REQUIRE(second != std::string::npos);
  issued out;
  REQUIRE(std::from_chars(got.out.data(), got.out.data() + first, out.seq).ec == std::errc{});
  out.path = got.out.substr(first + 1, second - first - 1);
  return out;
}

/// @brief Waits (bounded) until `db` holds `count` history rows, so a detached
/// submitter has finished before its arena is removed.
void await_history(const std::filesystem::path& db, std::int64_t count) {
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    auto opened = planar::db::connection::open_read_only(db.string());
    if (opened) {
      if (auto stmt = opened->prepare("select count(*) from queue_history"); stmt) {
        if (auto stepped = stmt->step(); stepped && *stepped == planar::db::step_result::row && stmt->column_int64(0) >= count) {
          return;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  FAIL("the detached submitter did not record its history row in " << db.string());
}

auto history_rows(const std::filesystem::path& db) -> std::vector<hq::history_row> {
  auto opened = planar::db::connection::open_read_only(db.string());
  REQUIRE(opened.has_value());
  auto rows = hq::list_history(*opened);
  REQUIRE(rows.has_value());
  return *rows;
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Happy path: a queued command runs with only PLANAR_DB and HOME pinned
// ---------------------------------------------------------------------------

TEST_CASE("queue run: runs against planar.db with its row there and no agent.db anywhere", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_happy");
  make_probe(arena);
  qfix::head_store(main_db(arena));

  auto const got = run_probe(arena, "happy", base_env(arena));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7); // The command's own status, passed through.
  CHECK(got.err.empty());
  CHECK(present(marker(arena)));

  auto const rows = history_rows(main_db(arena));
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().outcome == hq::history_outcome::exited);
  CHECK(rows.front().exit_code == 7);
  CHECK(rows.front().seq > 1'000'000); // The migration's sequence floor.
  CHECK_FALSE(present(arena.cpp_root / "agent.db"));
  CHECK_FALSE(present(arena.cpp_root / "agent.db-wal"));
}

TEST_CASE("queue run: a set PLANAR_AGENT_DB is ignored", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_ignored");
  make_probe(arena);
  qfix::head_store(main_db(arena));
  auto const elsewhere = arena.cpp_root / "elsewhere" / "agent.db";

  auto env = base_env(arena);
  set_var(env, "PLANAR_AGENT_DB", elsewhere.string());
  auto const got = run_probe(arena, "ignored", env);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7);
  CHECK(got.err.empty());
  CHECK(history_rows(main_db(arena)).size() == 1);
  CHECK_FALSE(present(elsewhere));
  CHECK_FALSE(present(elsewhere.parent_path()));
}

TEST_CASE("queue run: the database path is PLANAR_DB, or $HOME/.planar/planar.db when it is unset",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_resolve");
  make_probe(arena);

  SECTION("only PLANAR_DB is set") {
    qfix::head_store(main_db(arena));
    auto env = base_env(arena);
    remove_var(env, "HOME");
    remove_var(env, "PLANAR_AGENT_DB");
    auto const got = run_probe(arena, "only_db", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 7);
    CHECK(history_rows(main_db(arena)).size() == 1);
  }
  SECTION("only HOME is set") {
    auto const home_db = arena.cpp_root / "fakehome" / ".planar" / "planar.db";
    qfix::head_store(home_db);
    auto env = base_env(arena);
    remove_var(env, "PLANAR_DB");
    auto const got = run_probe(arena, "only_home", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 7);
    CHECK(history_rows(home_db).size() == 1);
    CHECK_FALSE(present(main_db(arena)));
  }
}

// ---------------------------------------------------------------------------
// Scenario: Error: the database cannot be opened
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a planar.db in a directory that cannot be written is used, since the directory is not written",
          "[cmd][agent][queue][queue-store]") {
  // The store is opened, never created, so what the refusal here depends on
  // is only whether the file exists: a missing one is refused, and the
  // read-only directory is left empty.
  auto const arena = parity::make_arena("qs_rodir");
  make_probe(arena);
  auto const dir = arena.cpp_root / "readonly";
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  mode_guard const restore{dir};
  if (!permissions_bind()) {
    note_permissions_do_not_bind();
    return;
  }

  auto const got = run_probe(arena, "rodir", env_for(arena, dir / "planar.db"));
  require_refused(arena, got, (dir / "planar.db").string());
  CHECK(got.err.find("planar init") != std::string::npos);
  CHECK_FALSE(present(dir / "planar.db"));
}

TEST_CASE("queue run: a database file that cannot be written refuses at 125 and runs nothing",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_rofile");
  make_probe(arena);
  auto const store = arena.cpp_root / "readonly.db";
  {
    std::ofstream out(store, std::ios::binary | std::ios::trunc);
  }
  std::filesystem::permissions(store, std::filesystem::perms::owner_read);
  mode_guard const restore{store};
  if (!permissions_bind()) {
    note_permissions_do_not_bind();
    return;
  }

  auto const got = run_probe(arena, "rofile", env_for(arena, store));
  require_refused(arena, got, store.string());
  CHECK(std::filesystem::file_size(store) == 0);
}

TEST_CASE("queue run: a database file that is a symbolic-link loop refuses at 125 and runs nothing, whoever runs it",
          "[cmd][agent][queue][queue-store]") {
  // The permission-bit cases above prove nothing as root, which ignores them.
  // Opening a loop fails with ELOOP for every user.
  auto const arena = parity::make_arena("qs_loopfile");
  make_probe(arena);
  auto const store = arena.cpp_root / "loop.db";
  std::filesystem::create_symlink(store, store);

  auto const got = run_probe(arena, "loopfile", env_for(arena, store));
  require_refused(arena, got, store.string());
  CHECK(std::filesystem::is_symlink(store));
}

TEST_CASE("queue run: a database in a directory that is a symbolic-link loop refuses at 125 and runs nothing, whoever runs it",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_loopdir");
  make_probe(arena);
  auto const dir = arena.cpp_root / "loopdir";
  std::filesystem::create_directory_symlink(dir, dir);

  auto const got = run_probe(arena, "loopdir", env_for(arena, dir / "planar.db"));
  require_refused(arena, got, "planar.db");
  CHECK(std::filesystem::is_symlink(dir));
}

TEST_CASE("queue run: a database path that is a directory refuses at 125 and runs nothing", "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_isdir");
  make_probe(arena);
  auto const store = arena.cpp_root / "store-is-a-directory";
  std::filesystem::create_directories(store);

  auto const got = run_probe(arena, "isdir", env_for(arena, store));
  require_refused(arena, got, store.string());
  CHECK(std::filesystem::is_directory(store));
  auto const json = status_json(arena, "isdir_status", env_for(arena, store));
  CHECK(json.code == 125);
  CHECK(has_tag(json, "store_unreachable"));
}

TEST_CASE("queue run: a database whose parent is a regular file refuses at 125 and runs nothing",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_parentfile");
  make_probe(arena);
  auto const blocker = arena.cpp_root / "blocker";
  {
    std::ofstream out(blocker, std::ios::binary | std::ios::trunc);
    out << "not a directory";
  }

  auto const got = run_probe(arena, "parentfile", env_for(arena, blocker / "planar.db"));
  require_refused(arena, got, "blocker");
  CHECK(read_all(blocker) == "not a directory");
}

TEST_CASE("queue run: a file that is not a database refuses at 125, is not called behind, and is left as it was",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_garbage");
  make_probe(arena);
  auto const store = arena.cpp_root / "garbage.db";
  auto const bytes = std::string(4096, 'x') + "this is not an SQLite database";
  {
    std::ofstream out(store, std::ios::binary | std::ios::trunc);
    out << bytes;
  }

  auto const got = run_probe(arena, "garbage", env_for(arena, store));
  require_refused(arena, got, store.string());
  CHECK(got.err.find("older than") == std::string::npos); // Not a version problem.
  CHECK(read_all(store) == bytes);
  auto const json = status_json(arena, "garbage_status", env_for(arena, store));
  CHECK(json.code == 125);
  CHECK(has_tag(json, "store_unreachable"));
}

// ---------------------------------------------------------------------------
// Scenario: Error: no database location can be found
// ---------------------------------------------------------------------------

TEST_CASE("queue run, cancel and status: neither PLANAR_DB nor HOME refuses at 125 with one error line, and claim keeps its code",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_noloc");
  make_probe(arena);
  auto env = base_env(arena);
  remove_var(env, "HOME");
  remove_var(env, "PLANAR_AGENT_DB");
  remove_var(env, "PLANAR_DB");

  auto const run = run_probe(arena, "noloc_run", env);
  require_refused(arena, run, "PLANAR_DB");
  CHECK(run.err.find("HOME") != std::string::npos);

  for (auto const& verb : std::vector<std::vector<std::string>>{{"queue", "cancel", "1"}, {"queue", "status", "1"}}) {
    INFO(verb[1]);
    auto const got = run_verb(arena, std::format("noloc_{}", verb[1]), verb, env);
    CHECK(got.code == 125);
    CHECK(got.out.empty());
    CHECK(got.err.starts_with("error: queue"));
    CHECK(std::ranges::count(got.err, '\n') == 1);
    CHECK(got.err.find("HOME") != std::string::npos);
  }

  // The claim ritual, in the same environment, keeps its pre-dispatch refusal.
  auto const claim = run_verb(arena, "noloc_claim", {"claim", "--entity", "task:1"}, env);
  CHECK(claim.code == 1);
  CHECK(claim.err.find("neither PLANAR_DB nor HOME is set; cannot locate the Planar database") != std::string::npos);

  CHECK_FALSE(present(arena.cpp_root / "agent.db"));
  CHECK_FALSE(present(main_db(arena)));
}

TEST_CASE("queue run and status: a fresh host with no planar.db refuses without creating it",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_fresh");
  make_probe(arena);
  auto const db = arena.cpp_root / "fresh" / "planar.db";

  auto const run = run_probe(arena, "fresh_run", env_for(arena, db));
  require_refused(arena, run, "planar init");
  auto const status = run_verb(arena, "fresh_status", {"queue", "status", "1"}, env_for(arena, db));
  CHECK(status.code == 125);
  CHECK(status.err.find("planar init") != std::string::npos);
  auto const json = status_json(arena, "fresh_json", env_for(arena, db));
  CHECK(json.code == 125);
  CHECK(has_tag(json, "store_unreachable"));
  auto const cancel = run_verb(arena, "fresh_cancel", {"queue", "cancel", "1"}, env_for(arena, db));
  CHECK(cancel.code == 125);

  for (auto const* suffix : {"", "-wal", "-shm"}) {
    CHECK_FALSE(present(std::filesystem::path{db.string() + suffix}));
  }
  CHECK_FALSE(present(db.parent_path()));
}

// ---------------------------------------------------------------------------
// Scenarios: the version handshake and the queue compatibility check
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a behind planar.db refuses at 125 naming planar init, with one line, and the file is untouched",
          "[cmd][agent][queue][queue-store][queue-schema]") {
  auto const arena = parity::make_arena("qs_behind");
  make_probe(arena);
  auto const db = main_db(arena);
  qfix::behind_store(db);
  auto const before = qfix::file_bytes(db);

  auto const got = run_probe(arena, "behind", env_for(arena, db));
  require_refused(arena, got, "run `planar init`");
  CHECK(got.err.find("error: schema version") == std::string::npos); // The holder's own line is folded in.
  CHECK(qfix::file_bytes(db) == before);
  CHECK_FALSE(present(std::filesystem::path{db.string() + "-wal"}));

  auto const json = status_json(arena, "behind_json", env_for(arena, db));
  CHECK(json.code == 125);
  CHECK(has_tag(json, "schema_version_behind"));
  CHECK(qfix::file_bytes(db) == before);
}

TEST_CASE("queue run: an incompatible ahead planar.db and a shape-drifted one refuse at 125 naming the cause, and write nothing",
          "[cmd][agent][queue][queue-schema]") {
  auto const arena = parity::make_arena("qs_incompat");
  make_probe(arena);

  SECTION("the marker needs a newer queue version") {
    auto const db = arena.cpp_root / "marker.db";
    qfix::incompatible_ahead_store(db);
    auto const got = run_probe(arena, "marker", env_for(arena, db));
    require_refused(arena, got, "queue version 2");
    CHECK(got.err.find(std::format("queue version {}", hq::k_queue_schema_version)) != std::string::npos);
    CHECK(qfix::queue_row_count(db) == 0);
    auto const json = status_json(arena, "marker_json", env_for(arena, db));
    CHECK(json.code == 125);
    CHECK(has_tag(json, "queue_schema_incompatible"));
    CHECK_FALSE(has_tag(json, "schema_version_ahead"));
  }
  SECTION("a queue column is missing and the marker does not say so") {
    auto const db = arena.cpp_root / "drift.db";
    qfix::shape_drift_store(db);
    auto const got = run_probe(arena, "drift", env_for(arena, db));
    require_refused(arena, got, "queue_entries.child_pgid");
    CHECK(qfix::queue_row_count(db) == 0);
    auto const json = status_json(arena, "drift_json", env_for(arena, db));
    CHECK(json.code == 125);
    CHECK(has_tag(json, "queue_schema_incompatible"));
  }
}

TEST_CASE("queue run: an equal-version planar.db without the queue tables is refused as foreign, in the line and in the JSON",
          "[cmd][agent][queue][queue-schema]") {
  auto const arena = parity::make_arena("qs_foreign");
  make_probe(arena);
  auto const db = main_db(arena);
  qfix::foreign_store(db);

  auto const got = run_probe(arena, "foreign", env_for(arena, db));
  require_refused(arena, got, "queue_schema");
  CHECK(got.err.find(std::format("migration {} is not this binary's migration {}", qfix::head_version(), qfix::head_version())) !=
        std::string::npos);
  auto const json = status_json(arena, "foreign_json", env_for(arena, db));
  CHECK(json.code == 125);
  CHECK(has_tag(json, "queue_schema_foreign"));
  CHECK_FALSE(has_tag(json, "queue_schema_incompatible"));
  CHECK_FALSE(has_tag(json, "schema_version_ahead"));
}

TEST_CASE("queue verbs: the version handshake runs before the queue check, so a behind database that also lost its queue "
          "tables is behind, not foreign",
          "[cmd][agent][queue][queue-schema]") {
  auto const arena = parity::make_arena("qs_order");
  auto const db    = main_db(arena);
  qfix::behind_store(db);
  qfix::exec(db, "drop table queue_schema");

  auto const json = status_json(arena, "order_json", env_for(arena, db));
  CHECK(json.code == 125);
  CHECK(has_tag(json, "schema_version_behind"));
  CHECK_FALSE(has_tag(json, "queue_schema_foreign"));
  CHECK_FALSE(has_tag(json, "queue_schema_incompatible"));
}

TEST_CASE("queue run, status and cancel: a compatible ahead planar.db is used as it is", "[cmd][agent][queue][queue-schema]") {
  auto const arena = parity::make_arena("qs_ahead");
  make_probe(arena);
  auto const db = main_db(arena);
  qfix::ahead_store(db);
  auto const versions = qfix::applied_versions(db);

  auto const got = run_probe(arena, "ahead", env_for(arena, db));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7);
  CHECK(got.err.empty());
  CHECK(present(marker(arena)));
  auto const rows = history_rows(db);
  REQUIRE(rows.size() == 1);

  auto const status =
      run_verb(arena, "ahead_status", {"queue", "status", std::to_string(rows.front().seq), "--json"}, env_for(arena, db));
  CHECK(status.code == 0);
  auto const cancel = run_verb(arena, "ahead_cancel", {"queue", "cancel", std::to_string(rows.front().seq)}, env_for(arena, db));
  CHECK(cancel.code == 6); // The entry has already ended: a verdict, not a refusal.
  CHECK(qfix::applied_versions(db) == versions);
}

TEST_CASE("queue status: reads planar.db when the file, its sidecars and its directory are all read-only",
          "[cmd][agent][queue][queue-store]") {
  // The store is opened read-only at the SQLite layer, so a database nobody can
  // write still answers (test spec 658, "queue status opens planar.db
  // read-only"). A connection kept open here holds the `-wal` and `-shm` in
  // place, so the case covers the sidecars being present and read-only too.
  auto const arena = parity::make_arena("qs_status_ro");
  make_probe(arena);
  // In a directory of its own: the harness writes its captures into the arena
  // root, which must stay writable.
  auto const db = arena.cpp_root / "ro" / "planar.db";
  qfix::head_store(db);
  auto const ran = run_probe(arena, "ro_seed", env_for(arena, db));
  REQUIRE(ran.code == 7);
  auto const rows = history_rows(db);
  REQUIRE(rows.size() == 1);
  auto const seq = rows.front().seq;

  auto holder = qfix::open_store(db);
  REQUIRE(holder.has_value());
  REQUIRE(holder->execute("select count(*) from queue_history").has_value()); // Brings the sidecars into being.

  auto const       wal = std::filesystem::path{db.string() + "-wal"};
  auto const       shm = std::filesystem::path{db.string() + "-shm"};
  auto const       dir = db.parent_path();
  mode_guard const restore_db{db};
  mode_guard const restore_dir{dir};
  mode_guard const restore_wal{wal};
  mode_guard const restore_shm{shm};
  if (!permissions_bind()) {
    note_permissions_do_not_bind();
    return;
  }
  REQUIRE(present(wal));
  REQUIRE(present(shm));
  auto const read_only =
      std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
  std::filesystem::permissions(db, read_only);
  std::filesystem::permissions(wal, read_only);
  std::filesystem::permissions(shm, read_only);
  std::filesystem::permissions(dir, read_only | std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                        std::filesystem::perms::others_exec);
  auto const before = qfix::file_bytes(db);

  auto const got = run_verb(arena, "ro_status", {"queue", "status", std::to_string(seq), "--json"}, env_for(arena, db));
  INFO("stdout:\n" << got.out << "stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out.find(std::format("\"seq\":{}", seq)) != std::string::npos);
  CHECK(got.out.find("\"outcome\":\"exited\"") != std::string::npos);
  CHECK(qfix::file_bytes(db) == before);
}

// ---------------------------------------------------------------------------
// Scenarios: where a detached run's log lives
// ---------------------------------------------------------------------------

TEST_CASE("queue run --detach: the log lives in queue-logs beside planar.db, and the help names the planar.db directory",
          "[cmd][agent][queue][queue-store][queue-logs]") {
  auto const arena = parity::make_arena("qs_log_default");
  qfix::head_store(main_db(arena));

  auto const got    = run_verb(arena, "log_default", {"queue", "run", "--detach", "--", "sh", "-c", "echo hi"}, base_env(arena));
  auto const ticket = read_ticket(got);
  CHECK(ticket.seq == 1'000'001);
  CHECK(ticket.path == (arena.cpp_root / "queue-logs" / "1000001.log").string());
  await_history(main_db(arena), 1);
  CHECK(read_all(ticket.path) == "hi\n");
  struct stat dir_info{};
  struct stat file_info{};
  REQUIRE(::stat(ticket.path.c_str(), &file_info) == 0);
  REQUIRE(::stat((arena.cpp_root / "queue-logs").c_str(), &dir_info) == 0);
  CHECK((dir_info.st_mode & 07777) == 0700);
  CHECK((file_info.st_mode & 07777) == 0600);

  auto const help = run_verb(arena, "log_help", {"queue", "run", "--help"}, base_env(arena));
  CHECK(help.code == 0);
  CHECK(help.out.find("<planar-db-directory>/queue-logs/<seq>.log") != std::string::npos);
  CHECK(help.out.find("agent-db") == std::string::npos);
}

TEST_CASE("queue run --detach: every database name gets its own stem-named log directory",
          "[cmd][agent][queue][queue-store][queue-logs]") {
  auto const arena = parity::make_arena("qs_log_stems");
  auto const dir   = arena.cpp_root / "dbs";

  struct row {
    std::string name;
    std::string logs;
  };
  for (auto const& c : std::vector<row>{{"planar.db", "queue-logs"},
                                        {"other.db", "other.queue-logs"},
                                        {"scratch.sqlite", "scratch.queue-logs"},
                                        {"queuedb", "queuedb.queue-logs"},
                                        {"a.b.db", "a.b.queue-logs"},
                                        {".hidden", ".hidden.queue-logs"}}) {
    INFO(c.name);
    auto const db = dir / c.name;
    qfix::head_store(db);
    auto const got    = run_verb(arena, "stem_" + std::to_string(std::hash<std::string>{}(c.name)),
                                 {"queue", "run", "--detach", "--", "true"}, env_for(arena, db));
    auto const ticket = read_ticket(got);
    CHECK(ticket.seq == 1'000'001);
    CHECK(ticket.path == (dir / c.logs / "1000001.log").string());
    CHECK(present(ticket.path));
    await_history(db, 1);
  }
}

TEST_CASE("queue run --detach: x.db and x.sqlite in one directory share a log directory, and the collision refuses without "
          "overwriting",
          "[cmd][agent][queue][queue-store][queue-logs]") {
  auto const arena = parity::make_arena("qs_log_collide");
  auto const dir   = arena.cpp_root / "dbs";
  qfix::head_store(dir / "x.db");
  qfix::head_store(dir / "x.sqlite");

  auto const first  = run_verb(arena, "collide_first", {"queue", "run", "--detach", "--", "sh", "-c", "echo first"},
                               env_for(arena, dir / "x.db"));
  auto const ticket = read_ticket(first);
  CHECK(ticket.path == (dir / "x.queue-logs" / "1000001.log").string());
  await_history(dir / "x.db", 1);
  auto const original = read_all(ticket.path);
  CHECK(original == "first\n");

  auto const second = run_verb(arena, "collide_second", {"queue", "run", "--detach", "--", "sh", "-c", "echo second"},
                               env_for(arena, dir / "x.sqlite"));
  INFO("stderr:\n" << second.err);
  CHECK(second.code == 125);
  CHECK(second.out.empty());
  CHECK(second.err.starts_with("error: queue: "));
  CHECK(std::ranges::count(second.err, '\n') == 1);
  CHECK(second.err.find(ticket.path) != std::string::npos);
  CHECK(read_all(ticket.path) == original);
}

TEST_CASE("queue run --detach: a log left by a counter reset refuses naming the file and the recovery section, and a "
          "moved-aside log lets the next run through",
          "[cmd][agent][queue][queue-store][queue-logs]") {
  auto const arena = parity::make_arena("qs_log_reset");
  qfix::head_store(main_db(arena));
  auto const logs = arena.cpp_root / "queue-logs";
  std::filesystem::create_directories(logs);
  std::filesystem::permissions(logs, std::filesystem::perms::owner_all);
  auto const old = logs / "1000001.log";
  {
    std::ofstream out(old, std::ios::binary);
    out << "old run\n";
  }

  auto const got = run_verb(arena, "reset_first", {"queue", "run", "--detach", "--", "true"}, base_env(arena));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err.starts_with("error: queue: "));
  CHECK(got.err.find(old.string()) != std::string::npos);
  CHECK(got.err.find("Host-queue rollback recovery") != std::string::npos);
  CHECK(got.err.find("migrations/README.md") != std::string::npos);
  CHECK(read_all(old) == "old run\n");
  CHECK(history_rows(main_db(arena)).empty());

  // Archived, as the recipe says: the name leaves the `<seq>.log` namespace.
  // The refused run consumed seq 1000001 from the counter, so the retry runs
  // as the next number whether or not the old log was moved; it is moved to
  // prove the recipe's end state is a clean directory.
  auto const archive = logs / "reset-archive-20260101T000000Z";
  std::filesystem::create_directories(archive);
  std::filesystem::rename(old, archive / "1000001.log");
  auto const again  = run_verb(arena, "reset_again", {"queue", "run", "--detach", "--", "true"}, base_env(arena));
  auto const ticket = read_ticket(again);
  CHECK(ticket.seq > 1'000'000);
  await_history(main_db(arena), 1);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: queue rule opens no database
// ---------------------------------------------------------------------------

TEST_CASE("queue rule: runs with PLANAR_DB, HOME and PLANAR_AGENT_DB all unset and creates nothing",
          "[cmd][agent][queue][queue-store]") {
  auto const arena = parity::make_arena("qs_rule");
  auto       env   = base_env(arena);
  remove_var(env, "HOME");
  remove_var(env, "PLANAR_AGENT_DB");
  remove_var(env, "PLANAR_DB");

  auto const got = run_verb(arena, "rule", {"queue", "rule"}, env);
  CHECK(got.code == 0);
  CHECK_FALSE(got.out.empty());
  CHECK_FALSE(present(main_db(arena)));
  CHECK_FALSE(present(arena.cpp_root / "agent.db"));
}
