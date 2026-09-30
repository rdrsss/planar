// @file agentstore.t.cpp
// @brief The viewer's agent database access policy (plan 1080, tasks 7024
// hq-watch-agentdb and 7092): read-only open, the agent compatibility check,
// a missing store as an empty queue, and reads that tolerate a store behind
// head. Expectations come from the tech spec (§ Agent database, "A binary
// refuses a store only when the highest row's compat is higher than its own
// schema version") and test-spec scenarios "the viewer opens the agent
// database read-only", "a missing store is an empty queue" and "the viewer
// refuses an incompatible store", not from the implementation.
//
// Every case builds its own scratch directory and environment map; nothing
// reads the process environment or touches ~/.planar.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.agentdb;
import planar.db.migrations_agent;
import planar.engine.hostqueue;
import planar.cmd.planar_watch.agentstore;
import planar.cmd.planar_watch.exit;

namespace {

namespace hq = planar::engine::hostqueue;
namespace fs = std::filesystem;

/// @brief A scratch directory removed on scope exit.
struct scratch {
  fs::path root;
  explicit scratch(std::string_view tag)
      : root(fs::temp_directory_path() /
             std::format("planar_watch_agentstore_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::error_code ec;
    fs::create_directories(root, ec);
  }
  scratch(const scratch&)                    = delete;
  auto operator=(const scratch&) -> scratch& = delete;
  ~scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

/// @brief The names of everything directly inside `dir`, sorted.
auto listing(const fs::path& dir) -> std::vector<std::string> {
  std::vector<std::string> names;
  for (auto const& e : fs::directory_iterator(dir)) {
    names.push_back(e.path().filename().string());
  }
  std::ranges::sort(names);
  return names;
}

/// @brief The whole file's bytes.
auto bytes_of(const fs::path& p) -> std::string {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Enqueues one waiting entry labelled `label` on `conn`.
auto add_entry(planar::db::connection& conn, std::string label) -> void {
  hq::enqueue_request request;
  request.host_id        = "h";
  request.pid            = 4242;
  request.pid_started    = 1;
  request.cwd            = "/work";
  request.argv           = {"make", "test"};
  request.label          = std::move(label);
  request.enqueued_at    = 1000;
  request.refreshed_mono = 5000;
  REQUIRE(hq::enqueue(conn, request).has_value());
}

/// @brief Creates a store at head with two waiting entries.
auto seed_head_store(const fs::path& store) -> void {
  auto opened = planar::db::agent::open_agent_db_at(store);
  REQUIRE(opened.has_value());
  add_entry(*opened, "first");
  add_entry(*opened, "second");
}

/// @brief Adds a version row claiming `version` / `compat` on top of the store.
auto stamp(const fs::path& store, std::uint32_t version, std::uint32_t compat) -> void {
  auto raw = planar::db::connection::open(store.string());
  REQUIRE(raw.has_value());
  REQUIRE(raw->execute(std::format("insert into agent_schema_migrations (version, compat, description) values ({}, {}, 'probe')",
                                   version, compat))
              .has_value());
}

} // namespace

TEST_CASE("a missing agent store reads as an empty queue and nothing is created", "[cmd][watch][agentstore]") {
  scratch    sc("missing");
  auto const store = sc.root / "agent.db";
  REQUIRE(listing(sc.root).empty());

  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE(opened.has_value());
  CHECK_FALSE(opened->present());
  CHECK(opened->connection() == nullptr);
  CHECK(opened->path() == store);

  auto const entries = opened->entries();
  REQUIRE(entries.has_value());
  CHECK(entries->empty());
  auto const history = opened->history();
  REQUIRE(history.has_value());
  CHECK(history->empty());
  auto const since = opened->history(123);
  REQUIRE(since.has_value());
  CHECK(since->empty());

  // No file, no -wal / -shm sidecar, nothing at all.
  CHECK(listing(sc.root).empty());
  CHECK_FALSE(fs::exists(store));
}

TEST_CASE("a missing store in a missing directory creates neither", "[cmd][watch][agentstore]") {
  scratch    sc("nodir");
  auto const store = sc.root / "never" / "created" / "agent.db";

  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE(opened.has_value());
  CHECK_FALSE(opened->present());
  CHECK(opened->entries().value().empty());
  CHECK_FALSE(fs::exists(sc.root / "never"));
}

TEST_CASE("the agent store path resolves like planar-agent's, and never from PLANAR_DB", "[cmd][watch][agentstore]") {
  scratch sc("resolve");

  SECTION("PLANAR_AGENT_DB wins") {
    auto const explicit_path = sc.root / "here" / "agent.db";
    auto opened = planar::cmd::watch::open_agent_store(planar::db::agent::map_env({{"PLANAR_AGENT_DB", explicit_path.string()},
                                                                                   {"HOME", (sc.root / "home").string()},
                                                                                   {"PLANAR_DB", "/nonexistent/x.db"}}));
    REQUIRE(opened.has_value());
    CHECK(opened->path() == explicit_path);
    CHECK_FALSE(opened->present());
  }
  SECTION("HOME falls back to ~/.planar/agent.db") {
    auto opened = planar::cmd::watch::open_agent_store(planar::db::agent::map_env({{"HOME", (sc.root / "home").string()}}));
    REQUIRE(opened.has_value());
    CHECK(opened->path() == sc.root / "home" / ".planar" / "agent.db");
    CHECK_FALSE(fs::exists(sc.root / "home"));
  }
  SECTION("nothing to resolve from is a failure with exit 1") {
    auto opened = planar::cmd::watch::open_agent_store(planar::db::agent::map_env({}));
    REQUIRE_FALSE(opened.has_value());
    CHECK(planar::cmd::watch::exit_code(opened.error()) == 1);
  }
}

TEST_CASE("a store at head is read, byte for byte unchanged by the read", "[cmd][watch][agentstore]") {
  scratch    sc("head");
  auto const store = sc.root / "agent.db";
  seed_head_store(store);
  auto const before_bytes = bytes_of(store);
  auto const before_time  = fs::last_write_time(store);

  {
    auto opened = planar::cmd::watch::open_agent_store_at(store);
    REQUIRE(opened.has_value());
    CHECK(opened->present());
    auto const entries = opened->entries();
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    CHECK(entries->front().label == "first");
    CHECK(entries->back().label == "second");
    CHECK(entries->front().argv == std::vector<std::string>{"make", "test"});
    CHECK(opened->history().value().empty());
  }
  CHECK(bytes_of(store) == before_bytes);
  CHECK((fs::last_write_time(store) == before_time));
  // SQLite itself creates the -wal / -shm sidecars when a read-only
  // connection opens a cleanly closed WAL store (the main database read by
  // planar-watch behaves the same); they carry no data, and nothing else may
  // appear.
  for (auto const& name : listing(sc.root)) {
    INFO("unexpected file: " << name);
    CHECK((name == "agent.db" || name == "agent.db-wal" || name == "agent.db-shm"));
  }
  if (fs::exists(store.string() + "-wal")) {
    CHECK(fs::file_size(store.string() + "-wal") == 0);
  }
}

TEST_CASE("a store with an uncheckpointed WAL write is read and its main file is untouched", "[cmd][watch][agentstore]") {
  scratch    sc("wal");
  auto const store = sc.root / "agent.db";
  seed_head_store(store);

  // A live writer holds the store open, so its journal is not checkpointed
  // away: the third row exists only in the -wal file.
  auto writer = planar::db::agent::open_agent_db_at(store);
  REQUIRE(writer.has_value());
  add_entry(*writer, "third-in-wal");
  REQUIRE(fs::exists(fs::path(store.string() + "-wal")));

  auto const before_bytes = bytes_of(store);
  auto const before_time  = fs::last_write_time(store);
  {
    auto opened = planar::cmd::watch::open_agent_store_at(store);
    REQUIRE(opened.has_value());
    auto const entries = opened->entries();
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 3);
    CHECK(entries->back().label == "third-in-wal");
  }
  CHECK(bytes_of(store) == before_bytes);
  CHECK((fs::last_write_time(store) == before_time));
}

TEST_CASE("any write through the handle is refused by SQLite", "[cmd][watch][agentstore][readonly]") {
  scratch    sc("readonly");
  auto const store = sc.root / "agent.db";
  seed_head_store(store);
  auto const before_bytes = bytes_of(store);

  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE(opened.has_value());
  auto* conn = opened->connection();
  REQUIRE(conn != nullptr);

  // Live handle: a read works, so the refusals below are not a dead handle.
  CHECK(conn->prepare("select count(*) from queue_entries").has_value());

  CHECK_FALSE(conn->execute("delete from queue_entries").has_value());
  CHECK_FALSE(conn->execute("insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) values "
                            "(9, 'exited', '/w', '[]', 1, 2, 0)")
                  .has_value());
  CHECK_FALSE(conn->execute("create table watch_probe (id integer primary key)").has_value());
  CHECK_FALSE(conn->execute("update agent_schema_migrations set compat = 1").has_value());
  CHECK(conn->is_read_only());

  CHECK(bytes_of(store) == before_bytes);
  CHECK(opened->entries().value().size() == 2);
}

TEST_CASE("a store ahead of the binary whose compat is not opens and reads", "[cmd][watch][agentstore]") {
  scratch    sc("ahead");
  auto const store = sc.root / "agent.db";
  seed_head_store(store);
  auto const head = planar::db::agent::agent_schema_version();
  stamp(store, head + 5, 1);

  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE(opened.has_value());
  CHECK(opened->present());
  CHECK(opened->entries().value().size() == 2);
}

TEST_CASE("an incompatible store is refused as schema-version-ahead, naming both values, and left alone",
          "[cmd][watch][agentstore]") {
  scratch    sc("incompat");
  auto const store = sc.root / "agent.db";
  seed_head_store(store);
  auto const head = planar::db::agent::agent_schema_version();
  stamp(store, head + 2, head + 1);
  auto const before_bytes = bytes_of(store);

  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::watch::domain_error_kind::schema_version_ahead);
  CHECK(planar::cmd::watch::exit_code(opened.error()) == 7);
  CHECK(opened.error().text.find(std::to_string(head + 1)) != std::string::npos);
  CHECK(opened.error().text.find(std::string("is ") + std::to_string(head)) != std::string::npos);
  CHECK(opened.error().text.find(store.string()) != std::string::npos);
  CHECK(bytes_of(store) == before_bytes);
}

TEST_CASE("a store still at agent schema version 2 reads with null limits and stays at version 2", "[cmd][watch][agentstore]") {
  // Task 7092. Built with raw SQL up to migration 00002, rows as that binary
  // wrote them: no code at head writes a version-2 store.
  scratch    sc("v2");
  auto const store = sc.root / "agent.db";
  auto const chain = planar::db::agent::migrations();
  REQUIRE(chain.size() >= 3);
  {
    auto raw = planar::db::connection::open(store.string());
    REQUIRE(raw.has_value());
    REQUIRE(planar::db::apply_all(*raw, chain.subspan(0, 2), planar::db::k_agent_version_table).has_value());
    REQUIRE(raw->execute("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, label, enqueued_at, "
                         "refreshed_mono, wait_deadline_mono) values ('waiting', 'h', 11, 111, '/w', '[\"make\"]', 'old-w', "
                         "1000, 5000, 9000);"
                         "insert into queue_history (seq, outcome, exit_code, cwd, argv, label, enqueued_at, started_at, "
                         "ended_at, waited_ms, ran_ms) values (7, 'exited', 0, '/w', '[\"true\"]', 'old-h', 900, 950, 990, "
                         "50, 40);")
                .has_value());
  }
  auto const before_bytes = bytes_of(store);

  {
    auto opened = planar::cmd::watch::open_agent_store_at(store);
    REQUIRE(opened.has_value());

    auto const entries = opened->entries();
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    CHECK(entries->front().label == "old-w");
    CHECK_FALSE(entries->front().run_limit_ms.has_value());
    CHECK_FALSE(entries->front().wait_limit_ms.has_value());

    auto const history = opened->history();
    REQUIRE(history.has_value());
    REQUIRE(history->size() == 1);
    CHECK(history->front().label == "old-h");
    CHECK(history->front().ran_ms == 40);
    CHECK_FALSE(history->front().run_limit_ms.has_value());
    CHECK_FALSE(history->front().wait_limit_ms.has_value());

    auto const filtered = opened->history(10'000);
    REQUIRE(filtered.has_value());
    CHECK(filtered->empty());

    CHECK(planar::db::current_version(*opened->connection(), planar::db::k_agent_version_table).value() == 2);
  }
  CHECK(bytes_of(store) == before_bytes);
}

TEST_CASE("an existing file with no queue tables yet reads as empty", "[cmd][watch][agentstore]") {
  // A submitter that has created the file and not yet migrated it.
  scratch    sc("fresh");
  auto const store = sc.root / "agent.db";
  {
    std::ofstream(store, std::ios::binary).flush();
  }
  REQUIRE(fs::file_size(store) == 0);

  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE(opened.has_value());
  CHECK(opened->present());
  CHECK(opened->entries().value().empty());
  CHECK(opened->history().value().empty());
  CHECK(fs::file_size(store) == 0);
}

TEST_CASE("a file that is not a SQLite database is an error, never an empty queue", "[cmd][watch][agentstore]") {
  scratch    sc("garbage");
  auto const store = sc.root / "agent.db";
  {
    std::ofstream out(store, std::ios::binary);
    out << "this is not a database, it is long enough to look like a header but is not one at all, padding padding padding";
  }
  auto opened = planar::cmd::watch::open_agent_store_at(store);
  REQUIRE_FALSE(opened.has_value());
  CHECK(planar::cmd::watch::exit_code(opened.error()) == 1);
  CHECK(opened.error().text.find(store.string()) != std::string::npos);
}
