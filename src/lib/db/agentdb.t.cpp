// @file agentdb.t.cpp
// @brief Unit tests for `planar.db.agentdb` (plan 1080, task
// hq-agentdb-open): the agent database's open path. Covers the test-spec
// scenarios "Happy path -- the agent database is created on first use",
// "Error -- an unwritable location is reported, not created elsewhere",
// "Edge -- neither HOME nor an explicit path is set" (the library half; the
// exit-125 mapping belongs to the `queue run` verb) and the library half of
// "Edge -- the queue works when the main database is unusable". The
// compatibility check (task hq-agentdb-compat) is covered at the end of the
// file: "Error -- a store that needs a newer binary is refused", "Edge -- a
// newer store that is still compatible is accepted" and "Error -- changing
// a migration's compat value fails a test" (the pinned compat table).
//
// Every case runs against a per-test scratch directory and passes its
// environment into the function under test through the injectable lookup,
// so nothing here reads or mutates the process environment and the suite
// stays hermetic under parallel ctest.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations_agent;
import planar.db.agentdb;

namespace {

/// @brief A unique scratch directory under the system temp directory,
/// removed (recursively, best-effort) when the guard goes out of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_agentdb_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    // A test may have removed write permission from a subdirectory; restore
    // it so remove_all can empty it.
    for (auto const& entry :
         std::filesystem::recursive_directory_iterator(path_, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (entry.is_directory(ec)) {
        std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
      }
    }
    std::filesystem::remove_all(path_, ec);
  }
};

/// @brief Every table the agent chain creates at head, sorted: the version
/// table (00001) and the two queue tables (00002, task hq-enqueue).
std::vector<std::string> const k_agent_tables{"agent_schema_migrations", "queue_entries", "queue_history"};

/// @brief Every user table on `conn` (SQLite's own `sqlite_*` bookkeeping
/// excluded), sorted.
auto user_tables(planar::db::connection& conn) -> std::vector<std::string> {
  std::vector<std::string> names;
  auto stmt = conn.prepare("select name from sqlite_master where type = 'table' and name not like 'sqlite_%' order by name");
  REQUIRE(stmt.has_value());
  while (stmt->step().value() == planar::db::step_result::row) {
    names.push_back(stmt->column_text(0));
  }
  return names;
}

/// @brief The first column of the first row of `sql`, as text.
auto scalar(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// @brief The whole file as bytes.
auto read_bytes(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// @brief Environment lookup over a fixed table; the test-side counterpart
/// of the process lookup, so a case never reads the real environment.
auto env_of(std::map<std::string, std::string, std::less<>> vars) -> planar::db::agent::env_lookup {
  return planar::db::agent::map_env(std::move(vars));
}

} // namespace

TEST_CASE("open_agent_db creates the agent database at PLANAR_AGENT_DB on first use, and a second open changes nothing",
          "[db][agentdb][hq-agentdb-open]") {
  // Test-spec "Happy path -- the agent database is created on first use".
  scratch_dir scratch;
  auto const  store = scratch.path_ / "nested" / "dir" / "agent.db";
  auto const  home  = scratch.path_ / "home";
  auto const  env   = env_of({{"PLANAR_AGENT_DB", store.string()}, {"HOME", home.string()}});
  REQUIRE_FALSE(std::filesystem::exists(store));

  auto const agent_head = planar::db::embedded_max(planar::db::agent::migrations());
  REQUIRE(agent_head >= 1);

  {
    auto opened = planar::db::agent::open_agent_db(env);
    REQUIRE(opened.has_value());
    CHECK(std::filesystem::is_regular_file(store));

    // Same connection settings as the main database.
    CHECK(scalar(*opened, "pragma journal_mode") == "wal");
    CHECK(scalar(*opened, "pragma busy_timeout") == "5000");
    CHECK(scalar(*opened, "pragma foreign_keys") == "1");
    CHECK_FALSE(opened->is_read_only());

    // At the current agent schema version, with nothing else in it.
    CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() == agent_head);
    CHECK(user_tables(*opened) == k_agent_tables);
    CHECK(scalar(*opened, "select count(*) from agent_schema_migrations") == std::to_string(agent_head));
    CHECK(planar::db::current_version(*opened, planar::db::k_main_version_table).value() == 0);
  }

  // The explicit path won: nothing appeared under HOME.
  CHECK_FALSE(std::filesystem::exists(home));

  // Opening again is a no-op: same version, no second application.
  {
    auto reopened = planar::db::agent::open_agent_db(env);
    REQUIRE(reopened.has_value());
    CHECK(planar::db::current_version(*reopened, planar::db::k_agent_version_table).value() == agent_head);
    CHECK(user_tables(*reopened) == k_agent_tables);
    CHECK(scalar(*reopened, "select count(*) from agent_schema_migrations") == std::to_string(agent_head));
    CHECK(scalar(*reopened, "pragma journal_mode") == "wal");
  }
}

TEST_CASE("open_agent_db migrates a store written at agent schema version 2 to head and keeps every queue row",
          "[db][agentdb][hq-queue-limit-columns]") {
  // A store an older planar-agent created and filled: the chain up to 00002,
  // one waiting and one running entry and one history row, written the way
  // that binary wrote them (it never names the limit columns). The rows are
  // seeded with raw SQL because no code at head writes a version-2 store.
  scratch_dir scratch;
  auto const  store = scratch.path_ / "agent.db";
  auto const  chain = planar::db::agent::migrations();
  REQUIRE(chain.size() >= 3);
  {
    auto conn = planar::db::connection::open(store.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 2), planar::db::k_agent_version_table).has_value());
    REQUIRE(conn->execute("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, label, enqueued_at, "
                          "refreshed_mono, wait_deadline_mono) values ('waiting', 'h', 11, 111, '/w', '[\"make\"]', 'old-w', "
                          "1000, 5000, 9000);"
                          "insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, label, enqueued_at, "
                          "started_at, refreshed_mono, deadline_mono) values ('running', 'h', 12, 112, '/w', '[\"ctest\"]', "
                          "'old-r', 1001, 1500, 5001, 7000);"
                          "insert into queue_history (seq, outcome, exit_code, cwd, argv, label, enqueued_at, started_at, "
                          "ended_at, waited_ms, ran_ms) values (7, 'exited', 0, '/w', '[\"true\"]', 'old-h', 900, 950, 990, "
                          "50, 40);")
                .has_value());
    REQUIRE(planar::db::current_version(*conn, planar::db::k_agent_version_table).value() == 2);
  }

  auto opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}}));
  REQUIRE(opened.has_value());
  CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() == planar::db::embedded_max(chain));

  CHECK(scalar(*opened, "select group_concat(seq || ':' || state || ':' || label || ':' || ifnull(deadline_mono, '-') || ':' || "
                        "ifnull(wait_deadline_mono, '-'), ',') from (select * from queue_entries order by seq)") ==
        "1:waiting:old-w:-:9000,2:running:old-r:7000:-");
  CHECK(scalar(*opened, "select group_concat(seq || ':' || outcome || ':' || label || ':' || waited_ms || ':' || ran_ms, ',') "
                        "from queue_history") == "7:exited:old-h:50:40");
  // What the older binary never recorded reads as unknown, not as zero.
  CHECK(scalar(*opened, "select count(*) from queue_entries where run_limit_ms is null and wait_limit_ms is null") == "2");
  CHECK(scalar(*opened, "select count(*) from queue_history where run_limit_ms is null and wait_limit_ms is null") == "1");
}

TEST_CASE("open_agent_db falls back to $HOME/.planar/agent.db and never reads PLANAR_DB as the store path",
          "[db][agentdb][hq-agentdb-open]") {
  scratch_dir scratch;
  auto const  home  = scratch.path_ / "home";
  auto const  decoy = scratch.path_ / "decoy-main.db";
  auto const  env   = env_of({{"HOME", home.string()}, {"PLANAR_DB", decoy.string()}});

  auto const resolved = planar::db::agent::resolve_agent_db_path(env);
  REQUIRE(resolved.has_value());
  CHECK(*resolved == home / ".planar" / "agent.db");

  auto opened = planar::db::agent::open_agent_db(env);
  REQUIRE(opened.has_value());
  CHECK(std::filesystem::is_regular_file(home / ".planar" / "agent.db"));
  CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() ==
        planar::db::embedded_max(planar::db::agent::migrations()));
  CHECK_FALSE(std::filesystem::exists(decoy));
}

TEST_CASE("open_agent_db migrates an existing store that is behind the embedded agent chain", "[db][agentdb][hq-agentdb-open]") {
  scratch_dir scratch;
  auto const  store = scratch.path_ / "agent.db";
  {
    // A bare SQLite file with no agent tables at all: version 0.
    auto conn = planar::db::connection::open(store.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("create table unrelated (id integer primary key);").has_value());
  }
  auto opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}}));
  REQUIRE(opened.has_value());
  CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() ==
        planar::db::embedded_max(planar::db::agent::migrations()));
  CHECK(user_tables(*opened) ==
        std::vector<std::string>{"agent_schema_migrations", "queue_entries", "queue_history", "unrelated"});
}

TEST_CASE("an unwritable agent database location is reported by path, and nothing is created at the fallback",
          "[db][agentdb][hq-agentdb-open]") {
  // Test-spec "Error -- an unwritable location is reported, not created
  // elsewhere".
  scratch_dir scratch;
  auto const  home = scratch.path_ / "home";

  SECTION("the parent of the configured path is a regular file, so the directory cannot be created") {
    auto const blocker = scratch.path_ / "blocker";
    {
      std::ofstream out(blocker);
      out << "not a directory\n";
    }
    auto const store  = blocker / "agent.db";
    auto       opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}, {"HOME", home.string()}}));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == planar::db::agent::open_error_kind::unwritable_location);
    CHECK(opened.error().path == store);
    CHECK(opened.error().message.find(store.string()) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(store));
  }

  SECTION("the parent directory exists but refuses writes") {
    auto const locked = scratch.path_ / "locked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    // A privileged process (root in a container) writes into 0500 anyway;
    // in that case this section proves nothing and says so, rather than
    // asserting an error the platform will not produce.
    bool const directory_is_writable = [&] {
      std::ofstream probe(locked / "probe");
      bool const    wrote = probe.is_open();
      probe.close();
      std::error_code ec;
      std::filesystem::remove(locked / "probe", ec);
      return wrote;
    }();
    auto const store  = locked / "agent.db";
    auto       opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}, {"HOME", home.string()}}));
    if (directory_is_writable) {
      WARN("running with write access to a 0500 directory; the unwritable-directory section cannot discriminate here");
      CHECK(opened.has_value());
    } else {
      REQUIRE_FALSE(opened.has_value());
      CHECK(opened.error().kind == planar::db::agent::open_error_kind::open_failed);
      CHECK(opened.error().path == store);
      CHECK(opened.error().message.find(store.string()) != std::string::npos);
      CHECK_FALSE(std::filesystem::exists(store));
    }
  }

  // Either way the fallback location was never touched.
  CHECK_FALSE(std::filesystem::exists(home));
}

TEST_CASE("resolve_agent_db_path fails naming the missing variables when neither PLANAR_AGENT_DB nor HOME is set",
          "[db][agentdb][hq-agentdb-open]") {
  // Test-spec "Edge -- neither HOME nor an explicit path is set": the
  // library returns the error value; the verb maps it to exit 125 later.
  SECTION("an empty environment") {
    auto const resolved = planar::db::agent::resolve_agent_db_path(env_of({}));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().kind == planar::db::agent::open_error_kind::unresolved_path);
    CHECK(resolved.error().path.empty());
    CHECK(resolved.error().message.find("PLANAR_AGENT_DB") != std::string::npos);
    CHECK(resolved.error().message.find("HOME") != std::string::npos);
  }

  SECTION("PLANAR_DB alone is not a fallback for the agent store") {
    scratch_dir scratch;
    auto const  main   = scratch.path_ / "planar.db";
    auto        opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_DB", main.string()}}));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == planar::db::agent::open_error_kind::unresolved_path);
    CHECK(opened.error().message.find("PLANAR_AGENT_DB") != std::string::npos);
    CHECK(opened.error().message.find("HOME") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(main));
    CHECK_FALSE(std::filesystem::exists(scratch.path_ / ".planar"));
  }

  SECTION("PLANAR_AGENT_DB set but empty is refused, not silently treated as unset") {
    scratch_dir scratch;
    auto const  home     = scratch.path_ / "home";
    auto const  resolved = planar::db::agent::resolve_agent_db_path(env_of({{"PLANAR_AGENT_DB", ""}, {"HOME", home.string()}}));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().kind == planar::db::agent::open_error_kind::unresolved_path);
    CHECK(resolved.error().message.find("PLANAR_AGENT_DB") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(home));
  }

  SECTION("HOME set but empty is treated as unset, so nothing resolves to /.planar/agent.db") {
    // Caveat from task hq-agentdb-open's review, pinned here (task
    // hq-agentdb-compat): an empty HOME must not produce a path at the
    // filesystem root.
    auto const resolved = planar::db::agent::resolve_agent_db_path(env_of({{"HOME", ""}}));
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().kind == planar::db::agent::open_error_kind::unresolved_path);
    CHECK(resolved.error().path.empty());
    CHECK(resolved.error().message.find("PLANAR_AGENT_DB") != std::string::npos);
    CHECK(resolved.error().message.find("HOME") != std::string::npos);
  }
}

TEST_CASE("open_agent_db succeeds while the main database is ahead of the binary, and leaves that file untouched",
          "[db][agentdb][hq-agentdb-open]") {
  // The library half of test-spec "Edge -- the queue works when the main
  // database is unusable": the agent open path never opens PLANAR_DB, so a
  // main-schema lockout cannot reach it.
  scratch_dir scratch;
  auto const  main  = scratch.path_ / "planar.db";
  auto const  store = scratch.path_ / "agent.db";
  {
    auto conn = planar::db::connection::open(main.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("create table schema_migrations (version integer primary key, applied_at text, description text);"
                          "insert into schema_migrations (version, description) values (999999, 'from the future');")
                .has_value());
    // Leave the decoy in rollback-journal mode. `connection::open` switches
    // a file to WAL, which rewrites header bytes 18-19 (1 -> 2), so an open
    // path that touched this file even harmlessly would change its bytes.
    REQUIRE(scalar(*conn, "pragma journal_mode = delete") == "delete");
  }
  REQUIRE_FALSE(std::filesystem::exists(main.string() + "-wal"));
  auto const before = read_bytes(main);
  REQUIRE(before.size() > 19);
  REQUIRE(before[18] == 1);
  REQUIRE(before[19] == 1);
  auto const main_mtime = std::filesystem::last_write_time(main);

  auto opened = planar::db::agent::open_agent_db(
      env_of({{"PLANAR_DB", main.string()}, {"PLANAR_AGENT_DB", store.string()}, {"HOME", (scratch.path_ / "home").string()}}));
  REQUIRE(opened.has_value());
  CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() ==
        planar::db::embedded_max(planar::db::agent::migrations()));
  CHECK(user_tables(*opened) == k_agent_tables);

  // Not opened at all: no sidecar appeared, and the bytes are the same.
  CHECK_FALSE(std::filesystem::exists(main.string() + "-wal"));
  CHECK_FALSE(std::filesystem::exists(main.string() + "-shm"));
  CHECK(read_bytes(main) == before);
  bool const mtime_unchanged = std::filesystem::last_write_time(main) == main_mtime;
  CHECK(mtime_unchanged);
}

TEST_CASE("the process environment lookup reads the same values as getenv", "[db][agentdb][hq-agentdb-open]") {
  // Read-only: PATH is set in every environment ctest runs under, and the
  // test mutates nothing.
  auto const env  = planar::db::agent::process_env();
  auto const path = env("PATH");
  REQUIRE(path.has_value());
  CHECK_FALSE(path->empty());
  CHECK_FALSE(env("PLANAR_AGENTDB_TEST_VARIABLE_THAT_IS_NEVER_SET").has_value());
}

namespace {

/// @brief Whether `table` exists on `conn`.
auto has_table(planar::db::connection& conn, std::string_view table) -> bool {
  auto stmt = conn.prepare(std::format("select count(*) from sqlite_master where type = 'table' and name = '{}'", table));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  return stmt->column_int64(0) == 1;
}

/// @brief A store at the head of the embedded agent chain, in WAL mode
/// (every agent store is: `open_agent_db_at` creates them through
/// `connection::open`), with one extra `agent_schema_migrations` row on
/// top claiming `version` / `compat`. Returns the file's bytes once every
/// connection to it is closed, so the caller can prove a later open left
/// it alone.
auto seed_store_ahead(const std::filesystem::path& store, std::uint32_t version, std::uint32_t compat) -> std::string {
  {
    auto seeded = planar::db::agent::open_agent_db_at(store);
    REQUIRE(seeded.has_value());
    REQUIRE(seeded
                ->execute(std::format("insert into agent_schema_migrations (version, compat, description) "
                                      "values ({}, {}, 'seeded by the test');",
                                      version, compat))
                .has_value());
  }
  // A closed WAL database has no sidecars: SQLite checkpoints and removes
  // them on the last close. A leftover here would mean the seed connection
  // is still alive and the byte comparison below would prove nothing.
  REQUIRE_FALSE(std::filesystem::exists(store.string() + "-wal"));
  REQUIRE_FALSE(std::filesystem::exists(store.string() + "-shm"));
  REQUIRE_FALSE(std::filesystem::exists(store.string() + "-journal"));
  return read_bytes(store);
}

} // namespace

TEST_CASE("open_agent_db_at refuses a store whose compat is above the binary's agent schema version, without modifying it",
          "[db][agentdb][hq-agentdb-compat]") {
  // Test-spec "Error -- a store that needs a newer binary is refused": the
  // row with the highest version is authoritative, and its compat above
  // this binary's version means a newer binary changed the meaning of
  // something this one would misread. The check runs before any migration
  // write, so the file is exactly what it was.
  scratch_dir scratch;
  auto const  store       = scratch.path_ / "agent.db";
  auto const  binary_head = planar::db::agent::agent_schema_version();
  REQUIRE(binary_head == planar::db::embedded_max(planar::db::agent::migrations()));

  auto const expect_refused = [&](std::string const& before, std::uint32_t store_compat) {
    auto const mtime  = std::filesystem::last_write_time(store);
    auto       opened = planar::db::agent::open_agent_db_at(store);
    REQUIRE_FALSE(opened.has_value());
    auto const& error = opened.error();
    CHECK(error.kind == planar::db::agent::open_error_kind::incompatible_store);
    CHECK(error.path == store);
    CHECK(error.store_compat == store_compat);
    CHECK(error.binary_version == binary_head);
    CHECK(error.message.find(store.string()) != std::string::npos);
    CHECK(error.message.find(std::to_string(store_compat)) != std::string::npos);
    CHECK(error.message.find(std::to_string(binary_head)) != std::string::npos);

    // Untouched: same bytes, no sidecar or journal left behind, same mtime.
    CHECK(read_bytes(store) == before);
    CHECK_FALSE(std::filesystem::exists(store.string() + "-wal"));
    CHECK_FALSE(std::filesystem::exists(store.string() + "-shm"));
    CHECK_FALSE(std::filesystem::exists(store.string() + "-journal"));
    bool const mtime_unchanged = std::filesystem::last_write_time(store) == mtime;
    CHECK(mtime_unchanged);
  };

  SECTION("the highest row's compat equals its version, one above the binary") {
    auto const before = seed_store_ahead(store, binary_head + 1, binary_head + 1);
    expect_refused(before, binary_head + 1);
    // Also refused through the environment-resolving entry point.
    auto opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}}));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == planar::db::agent::open_error_kind::incompatible_store);
    CHECK(read_bytes(store) == before);
  }

  SECTION("the highest row's compat is above the binary but below its own version") {
    auto const before = seed_store_ahead(store, binary_head + 5, binary_head + 2);
    expect_refused(before, binary_head + 2);
  }

  SECTION("only the highest row counts: a lower row with a low compat does not rescue the store") {
    // The seed leaves the whole embedded chain in place (every compat there
    // is <= binary_head); the extra row on top is what decides.
    auto const before = seed_store_ahead(store, binary_head + 1, binary_head + 1);
    {
      auto raw = planar::db::connection::open(store.string());
      REQUIRE(raw.has_value());
      CHECK(scalar(*raw, "select min(compat) from agent_schema_migrations") == "1");
    }
    REQUIRE(read_bytes(store) == before);
    expect_refused(before, binary_head + 1);
  }

  // The refusal applied no migration and wrote nothing: the seeded row set is
  // exactly what the store holds.
  auto raw = planar::db::connection::open(store.string());
  REQUIRE(raw.has_value());
  CHECK(scalar(*raw, "select count(*) from agent_schema_migrations") == std::to_string(binary_head + 1));
}

TEST_CASE("open_agent_db_at accepts a store that is ahead of the binary but whose compat is not, and reports its version",
          "[db][agentdb][hq-agentdb-compat]") {
  // Test-spec "Edge -- a newer store that is still compatible is accepted":
  // a newer binary added something and kept compat, so this binary reads
  // the store it understands and leaves the newer rows alone.
  scratch_dir scratch;
  auto const  store       = scratch.path_ / "agent.db";
  auto const  binary_head = planar::db::agent::agent_schema_version();

  SECTION("one version ahead, compat unchanged") {
    seed_store_ahead(store, binary_head + 1, binary_head);
    auto opened = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}}));
    REQUIRE(opened.has_value());
    CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() == binary_head + 1);
    CHECK(scalar(*opened, "select count(*) from agent_schema_migrations") == std::to_string(binary_head + 1));
    CHECK(scalar(*opened, "pragma journal_mode") == "wal");
    CHECK_FALSE(opened->is_read_only());
    // Usable: a write on the opened connection succeeds and is visible.
    REQUIRE(opened->execute("create table hq_compat_probe (id integer primary key); insert into hq_compat_probe values (1);")
                .has_value());
    CHECK(scalar(*opened, "select count(*) from hq_compat_probe") == "1");
  }

  SECTION("several versions ahead, compat below the binary's version") {
    seed_store_ahead(store, binary_head + 3, binary_head);
    auto opened = planar::db::agent::open_agent_db_at(store);
    REQUIRE(opened.has_value());
    CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() == binary_head + 3);
  }

  SECTION("the direct check agrees with the open path") {
    seed_store_ahead(store, binary_head + 1, binary_head);
    auto raw = planar::db::connection::open(store.string());
    REQUIRE(raw.has_value());
    CHECK(planar::db::agent::check_compat(*raw, store).has_value());
    REQUIRE(raw->execute(std::format("update agent_schema_migrations set compat = {} where version = {};", binary_head + 1,
                                     binary_head + 1))
                .has_value());
    auto const refused = planar::db::agent::check_compat(*raw, store);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == planar::db::agent::open_error_kind::incompatible_store);
    CHECK(refused.error().store_compat == binary_head + 1);
    CHECK(refused.error().binary_version == binary_head);
  }
}

TEST_CASE("check_compat accepts a fresh store and a store at or behind the binary", "[db][agentdb][hq-agentdb-compat]") {
  scratch_dir scratch;
  auto const  store = scratch.path_ / "agent.db";
  auto        raw   = planar::db::connection::open(store.string());
  REQUIRE(raw.has_value());

  // Fresh: no version table at all.
  REQUIRE_FALSE(has_table(*raw, "agent_schema_migrations"));
  CHECK(planar::db::agent::check_compat(*raw, store).has_value());

  // At head.
  REQUIRE(planar::db::apply_contiguous(*raw, planar::db::agent::migrations(), planar::db::k_agent_version_table).has_value());
  CHECK(planar::db::agent::check_compat(*raw, store).has_value());

  // Behind (an empty version table reads as version 0, compat none).
  REQUIRE(raw->execute("delete from agent_schema_migrations;").has_value());
  CHECK(planar::db::agent::check_compat(*raw, store).has_value());
}

TEST_CASE("every agent migration's compat value is pinned", "[db][agentdb][migrations][hq-agentdb-compat]") {
  // Test-spec "Error -- changing a migration's compat value fails a test".
  // The table below IS the reviewed contract (migrations-agent/README.md):
  // an additive migration keeps the previous compat, one that drops,
  // renames or changes meaning sets compat to its own version. A new
  // migration must be added here; a changed value must be changed here.
  //
  // The actual value is what the migration's up SQL inserts, read back
  // after applying the chain one migration at a time to a scratch store,
  // so a migration whose insert disagrees with its own filename or with
  // this table is caught regardless of how the insert is spelled.
  static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 3> k_pinned_compat{{
      {"00001_agent_foundation.up.sql", 1},
      {"00002_queue_tables.up.sql", 1}, // additive: two tables and their indexes (task hq-enqueue)
      {"00003_queue_limits.up.sql", 1}, // additive: four nullable columns (task hq-queue-limit-columns)
  }};

  scratch_dir scratch;
  auto        conn = planar::db::connection::open((scratch.path_ / "agent.db").string());
  REQUIRE(conn.has_value());

  auto const chain = planar::db::agent::migrations();
  REQUIRE_FALSE(chain.empty());
  REQUIRE(planar::db::require_contiguous(chain).has_value());

  std::set<std::string, std::less<>> seen;
  std::uint32_t                      previous_compat = 0;
  for (std::size_t i = 0; i < chain.size(); ++i) {
    auto const& record = chain[i];
    auto const  file   = std::format("{:05}_{}.up.sql", record.version_, record.name_);
    INFO("agent migration file " << file);
    CHECK(std::filesystem::is_regular_file(std::filesystem::path(PLANAR_MIGRATIONS_AGENT_DIR) / file));
    seen.insert(file);

    REQUIRE(planar::db::apply_all(*conn, chain.subspan(i, 1), planar::db::k_agent_version_table).has_value());
    auto row = conn->prepare("select compat from agent_schema_migrations where version = ?");
    REQUIRE(row.has_value());
    REQUIRE(row->bind_int64(1, record.version_).has_value());
    REQUIRE(row->step().value() == planar::db::step_result::row);
    auto const actual = static_cast<std::uint32_t>(row->column_int64(0));

    auto const pinned = std::ranges::find(k_pinned_compat, file, &std::pair<std::string_view, std::uint32_t>::first);
    if (pinned == k_pinned_compat.end()) {
      FAIL("agent migration " << file << " is missing from the pinned compat table (its compat is " << actual << ")");
    }
    if (actual != pinned->second) {
      FAIL("agent migration " << file << " inserts compat " << actual << " but the pinned compat table says " << pinned->second);
    }
    // The rule itself: keep the previous compat, or set it to this version.
    CHECK((actual == previous_compat || actual == record.version_));
    CHECK(actual >= previous_compat);
    CHECK(actual <= record.version_);
    previous_compat = actual;
  }

  for (auto const& [file, compat] : k_pinned_compat) {
    INFO("pinned entry " << file << " -> " << compat);
    if (!seen.contains(file)) {
      FAIL("the pinned compat table names " << file << ", which is not an embedded agent migration");
    }
  }
  CHECK(seen.size() == k_pinned_compat.size());
}

TEST_CASE("open_agent_db_read_only_at opens strictly read-only and never creates the file, its directory or a migration",
          "[db][agentdb][hq-queue-status]") {
  scratch_dir scratch;
  auto const  store = scratch.path_ / "nested" / "agent.db";

  SECTION("a missing store is refused by path and nothing is created") {
    auto opened = planar::db::agent::open_agent_db_read_only_at(store);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == planar::db::agent::open_error_kind::open_failed);
    CHECK(opened.error().path == store);
    CHECK(opened.error().message.find(store.string()) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(store));
    CHECK_FALSE(std::filesystem::exists(store.parent_path()));
  }

  SECTION("an existing store opens read-only and refuses every write") {
    {
      auto created = planar::db::agent::open_agent_db_at(store);
      REQUIRE(created.has_value());
    }
    auto const before = read_bytes(store);
    auto       opened = planar::db::agent::open_agent_db_read_only_at(store);
    REQUIRE(opened.has_value());
    CHECK(opened->is_read_only());
    CHECK_FALSE(
        opened->execute("insert into agent_schema_migrations (version, compat, description) values (99, 1, 'x')").has_value());
    CHECK_FALSE(opened->execute("create table extra (a integer)").has_value());
    opened = std::unexpected(planar::db::agent::open_error{}); // close it
    CHECK(read_bytes(store) == before);
  }

  SECTION("a store that needs a newer binary is refused, as the read-write open refuses it") {
    auto const binary_head = planar::db::agent::agent_schema_version();
    static_cast<void>(seed_store_ahead(store, binary_head + 1, binary_head + 1));
    auto opened = planar::db::agent::open_agent_db_read_only_at(store);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == planar::db::agent::open_error_kind::incompatible_store);
  }

  SECTION("the environment-resolving form reads the same store, and reports an unresolvable path") {
    {
      auto created = planar::db::agent::open_agent_db_at(store);
      REQUIRE(created.has_value());
    }
    auto opened = planar::db::agent::open_agent_db_read_only(env_of({{"PLANAR_AGENT_DB", store.string()}}));
    REQUIRE(opened.has_value());
    CHECK(opened->is_read_only());
    auto const unresolved = planar::db::agent::open_agent_db_read_only(env_of({}));
    REQUIRE_FALSE(unresolved.has_value());
    CHECK(unresolved.error().kind == planar::db::agent::open_error_kind::unresolved_path);
  }
}

// ---------------------------------------------------------------------------
// Decision 1210 (task hq-agentdb-file-modes): the agent database holds claim
// tokens, so it is created owner-only, and so is the directory this module
// creates for it.
// ---------------------------------------------------------------------------

namespace {

/// @brief The permission bits of `path` (no type bits), or 0xFFFF when it
/// cannot be read.
auto mode_bits(const std::filesystem::path& path) -> unsigned {
  struct ::stat info{};
  if (::stat(path.c_str(), &info) != 0) {
    return 0xFFFFU;
  }
  return static_cast<unsigned>(info.st_mode) & 07777U;
}

/// @brief Sets the process umask to the usual 022 for one case and restores
/// the previous value, so a file's mode is never 0600 merely because the
/// environment's umask happened to be strict.
struct usual_umask {
  ::mode_t previous;
  usual_umask() : previous(::umask(022)) {
  }
  ~usual_umask() {
    ::umask(previous);
  }
  usual_umask(const usual_umask&)            = delete;
  usual_umask& operator=(const usual_umask&) = delete;
};

} // namespace

TEST_CASE("a created agent database is owner-only, with its -wal and -shm", "[db][agentdb][hq-agentdb-modes]") {
  usual_umask umask_guard;
  scratch_dir scratch;
  auto const  store = scratch.path_ / "agent.db";

  auto opened = planar::db::agent::open_agent_db_at(store);
  REQUIRE(opened.has_value());
  // A write keeps the WAL sidecars in existence for as long as the connection
  // is open; SQLite creates them with the main file's mode.
  REQUIRE(opened->execute("create table if not exists modes_probe (a integer)").has_value());
  REQUIRE(opened->execute("insert into modes_probe values (1)").has_value());

  CHECK(mode_bits(store) == 0600U);
  auto const wal = std::filesystem::path{store.string() + "-wal"};
  auto const shm = std::filesystem::path{store.string() + "-shm"};
  REQUIRE(std::filesystem::exists(wal));
  REQUIRE(std::filesystem::exists(shm));
  CHECK(mode_bits(wal) == 0600U);
  CHECK(mode_bits(shm) == 0600U);
}

TEST_CASE("the directories the agent database open path creates are 0700; an existing one is left alone",
          "[db][agentdb][hq-agentdb-modes]") {
  usual_umask umask_guard;
  scratch_dir scratch;

  SECTION("every missing component of an explicit path is created 0700") {
    auto const store = scratch.path_ / "a" / "b" / "agent.db";
    auto       env   = env_of({{"PLANAR_AGENT_DB", store.string()}});
    auto       open  = planar::db::agent::open_agent_db(env);
    REQUIRE(open.has_value());
    CHECK(mode_bits(scratch.path_ / "a") == 0700U);
    CHECK(mode_bits(scratch.path_ / "a" / "b") == 0700U);
    CHECK(mode_bits(store) == 0600U);
  }

  SECTION("a directory that already exists, chosen by the user, is never chmodded") {
    auto const shared = scratch.path_ / "shared";
    std::filesystem::create_directories(shared);
    ::chmod(shared.c_str(), 0755);
    auto const store = shared / "agent.db";
    auto       open  = planar::db::agent::open_agent_db(env_of({{"PLANAR_AGENT_DB", store.string()}}));
    REQUIRE(open.has_value());
    CHECK(mode_bits(shared) == 0755U);
    CHECK(mode_bits(store) == 0600U);
  }

  SECTION("the default location creates $HOME/.planar 0700") {
    auto const home = scratch.path_ / "home";
    std::filesystem::create_directories(home);
    auto open = planar::db::agent::open_agent_db(env_of({{"HOME", home.string()}}));
    REQUIRE(open.has_value());
    CHECK(mode_bits(home / ".planar") == 0700U);
    CHECK(mode_bits(home / ".planar" / "agent.db") == 0600U);
    CHECK(mode_bits(home) != 0700U); // only Planar's own directory is ensured
  }

  SECTION("the default location tightens an existing $HOME/.planar to 0700") {
    auto const planar_home = scratch.path_ / "home" / ".planar";
    std::filesystem::create_directories(planar_home);
    ::chmod(planar_home.c_str(), 0755);
    auto open = planar::db::agent::open_agent_db(env_of({{"HOME", (scratch.path_ / "home").string()}}));
    REQUIRE(open.has_value());
    CHECK(mode_bits(planar_home) == 0700U);
  }
}
