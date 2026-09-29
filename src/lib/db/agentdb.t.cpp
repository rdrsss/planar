// @file agentdb.t.cpp
// @brief Unit tests for `planar.db.agentdb` (plan 1080, task
// hq-agentdb-open): the agent database's open path. Covers the test-spec
// scenarios "Happy path -- the agent database is created on first use",
// "Error -- an unwritable location is reported, not created elsewhere",
// "Edge -- neither HOME nor an explicit path is set" (the library half; the
// exit-125 mapping belongs to the `queue run` verb) and the library half of
// "Edge -- the queue works when the main database is unusable".
//
// Every case runs against a per-test scratch directory and passes its
// environment into the function under test through the injectable lookup,
// so nothing here reads or mutates the process environment and the suite
// stays hermetic under parallel ctest.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

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
    CHECK(user_tables(*opened) == std::vector<std::string>{"agent_schema_migrations"});
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
    CHECK(user_tables(*reopened) == std::vector<std::string>{"agent_schema_migrations"});
    CHECK(scalar(*reopened, "select count(*) from agent_schema_migrations") == std::to_string(agent_head));
    CHECK(scalar(*reopened, "pragma journal_mode") == "wal");
  }
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
  CHECK(user_tables(*opened) == std::vector<std::string>{"agent_schema_migrations", "unrelated"});
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
  CHECK(user_tables(*opened) == std::vector<std::string>{"agent_schema_migrations"});

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
