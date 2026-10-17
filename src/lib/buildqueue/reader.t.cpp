// @file reader.t.cpp
// @brief Unit tests for `planar.buildqueue.reader`. Every case builds its own
// build queue database, or a planar.db, under a per-test scratch directory,
// then reads it through the reader's public entry points. The write-refusal
// checks open the reader's own connection through `open_reader`, so a removed
// `pragma query_only` fails them.

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.buildqueue.reader;
import planar.db;
import planar.db.migrate;

namespace {

using planar::buildqueue::open_reader;
using planar::buildqueue::read_queue_facts;
using planar::buildqueue::read_tagged_runs;
using planar::buildqueue::reader_error_kind;
using planar::buildqueue::reader_state;

/// @brief A per-test scratch directory under the system temp directory, removed on scope exit.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir() {
    static std::atomic<int> counter{0};
    auto const              stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_                         = std::filesystem::temp_directory_path() /
                                    std::format("planar_buildqueue_reader_{}_{}_{}", ::getpid(), stamp, counter.fetch_add(1));
    std::filesystem::create_directories(path_);
  }
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  /// @brief The path of a file named `name` inside this directory.
  auto file(std::string_view name) const -> std::filesystem::path {
    return path_ / name;
  }
};

/// @brief Creates `path` with owner-only mode, empty, so a database created in it inherits that mode.
/// @param path The file to create.
void create_owner_only(const std::filesystem::path& path) {
  {
    std::ofstream out(path, std::ios::binary);
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

/// @brief Opens `path` read-write, which creates it, and applies the build queue chain.
/// @param path The database file.
/// @return The migrated connection.
auto make_queue_db(const std::filesystem::path& path) -> planar::db::connection {
  auto conn = planar::db::connection::open(path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_build_queue(*conn).has_value());
  return std::move(*conn);
}

/// @brief Sets one `queue_settings` row, inserting or replacing it.
/// @param conn A read-write connection to the queue database.
/// @param value The stored value for `tagged_history_days`.
void set_tagged_days(planar::db::connection& conn, std::string_view value) {
  auto stmt = conn.prepare("insert or replace into queue_settings (key, value) values ('tagged_history_days', ?1)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, value).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Inserts one `queue_history` row.
/// @param conn A read-write connection to the queue database.
/// @param seq The sequence number of the ended entry.
/// @param tag The tag stored on the row.
/// @param argv_json The stored argv text, a JSON array.
/// @param outcome The outcome text.
/// @param exit_code The exit status, or nullopt for none.
/// @param ended_at Wall clock at the end, in milliseconds.
void insert_run(planar::db::connection& conn, std::int64_t seq, std::string_view tag, std::string_view argv_json,
                std::string_view outcome, std::optional<std::int64_t> exit_code, std::int64_t ended_at) {
  auto stmt = conn.prepare("insert into queue_history (seq, outcome, exit_code, cwd, argv, tag, enqueued_at, ended_at, "
                           "waited_ms) values (?1, ?2, ?3, '/work', ?4, ?5, 0, ?6, 0)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, seq).has_value());
  REQUIRE(stmt->bind_text(2, outcome).has_value());
  if (exit_code) {
    REQUIRE(stmt->bind_int64(3, *exit_code).has_value());
  } else {
    REQUIRE(stmt->bind_null(3).has_value());
  }
  REQUIRE(stmt->bind_text(4, argv_json).has_value());
  REQUIRE(stmt->bind_text(5, tag).has_value());
  REQUIRE(stmt->bind_int64(6, ended_at).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief The stored `queue_identity.created_at`.
/// @param conn Any connection to the queue database.
/// @return The creation time text.
auto identity_created_at(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select created_at from queue_identity");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  return stmt->column_text(0);
}

/// @brief Reads a whole file as bytes.
/// @param path The file.
/// @return Its contents.
auto file_bytes(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Restores a file's owner-only mode when it leaves scope, so the scratch directory can be removed.
struct restore_owner_mode {
  std::filesystem::path path_;
  ~restore_owner_mode() {
    std::error_code ec;
    std::filesystem::permissions(path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
  }
};

} // namespace

TEST_CASE("reader: returns the tagged runs, creation time and retention, and writes nothing", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("queue.db");
  std::string created_at;
  {
    auto conn  = make_queue_db(path);
    created_at = identity_created_at(conn);
    set_tagged_days(conn, "30");
    insert_run(conn, 2000001, "T", R"(["make","test"])", "exited", 0, 1000);
    insert_run(conn, 2000002, "other", R"(["make","lint"])", "exited", 0, 1100);
    insert_run(conn, 2000003, "T", R"(["make","build"])", "timeout", std::nullopt, 900);
    insert_run(conn, 2000004, "T", R"(["echo","q\"uote\\back\u0001é😀"])", "cancelled", 130, 2000);
  }
  auto const before = file_bytes(path);

  auto const runs = read_tagged_runs(path, "T");
  REQUIRE(runs.has_value());
  CHECK(runs->state == reader_state::present);
  REQUIRE(runs->created_at.has_value());
  CHECK(*runs->created_at == created_at);
  CHECK(runs->tagged_history_days == 30);

  // Ordered by ended_at: the 900 ms run (seq 2000003) comes before the 1000 ms run.
  REQUIRE(runs->runs.size() == 3);
  CHECK(runs->runs[0].argv == std::vector<std::string>{"make", "build"});
  CHECK(runs->runs[0].outcome == "timeout");
  CHECK_FALSE(runs->runs[0].exit_code.has_value());
  CHECK(runs->runs[0].cwd == "/work");
  CHECK(runs->runs[0].ended_at == 900);
  CHECK(runs->runs[1].argv == std::vector<std::string>{"make", "test"});
  CHECK(runs->runs[1].outcome == "exited");
  REQUIRE(runs->runs[1].exit_code.has_value());
  CHECK(*runs->runs[1].exit_code == 0);
  // The escaped argv decodes as the engine's encoder wrote it.
  CHECK(runs->runs[2].argv == std::vector<std::string>{"echo", std::string("q\"uote\\back\x01\xC3\xA9\xF0\x9F\x98\x80", 18)});
  CHECK(runs->runs[2].outcome == "cancelled");
  REQUIRE(runs->runs[2].exit_code.has_value());
  CHECK(*runs->runs[2].exit_code == 130);

  auto const facts = read_queue_facts(path);
  REQUIRE(facts.has_value());
  CHECK(facts->state == reader_state::present);
  REQUIRE(facts->created_at.has_value());
  CHECK(*facts->created_at == created_at);
  CHECK(facts->tagged_history_days == 30);

  // The reader's connection refuses a write: query_only is on before any read.
  {
    auto conn = open_reader(path);
    REQUIRE(conn.has_value());
    auto write = conn->execute("insert into queue_settings (key, value) values ('slots', '4')");
    REQUIRE_FALSE(write.has_value());
    CHECK(planar::db::is_access_failure(write.error()));
  }

  CHECK(file_bytes(path) == before);
}

TEST_CASE("reader: a missing or invalid retention setting reads as 90 days", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("queue.db");
  auto        conn = make_queue_db(path);

  // No settings row at all.
  auto missing = read_queue_facts(path);
  REQUIRE(missing.has_value());
  CHECK(missing->tagged_history_days == 90);

  // A value that is not a non-negative integer.
  for (std::string_view invalid : {"soon", "-5", "", "30 days"}) {
    set_tagged_days(conn, invalid);
    auto facts = read_queue_facts(path);
    REQUIRE(facts.has_value());
    CHECK(facts->tagged_history_days == 90);
  }
}

TEST_CASE("reader: a behind database and an ahead one with the needed columns both read", "[buildqueue][reader]") {
  scratch_dir scratch;

  // Behind: only the identity row, the settings table and the history columns the reader reads.
  auto const behind = scratch.file("behind.db");
  {
    auto conn = planar::db::connection::open(behind.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("create table queue_identity (kind text primary key, created_at text not null)").has_value());
    REQUIRE(conn->execute("insert into queue_identity (kind, created_at) values ('planar-build-queue', "
                          "'2026-10-01T00:00:00.000Z')")
                .has_value());
    REQUIRE(conn->execute("create table queue_settings (key text primary key, value text not null)").has_value());
    REQUIRE(conn->execute("create table queue_history (seq integer primary key, outcome text not null, exit_code "
                          "integer, cwd text not null, argv text not null, tag text, ended_at integer not null)")
                .has_value());
    REQUIRE(conn->execute("insert into queue_history (seq, outcome, exit_code, cwd, argv, tag, ended_at) "
                          "values (7, 'exited', 0, '/w', '[\"ok\"]', 'T', 5)")
                .has_value());
  }
  auto const old_runs = read_tagged_runs(behind, "T");
  REQUIRE(old_runs.has_value());
  CHECK(old_runs->state == reader_state::present);
  CHECK(old_runs->created_at.value_or("") == "2026-10-01T00:00:00.000Z");
  CHECK(old_runs->tagged_history_days == 90);
  REQUIRE(old_runs->runs.size() == 1);
  CHECK(old_runs->runs[0].argv == std::vector<std::string>{"ok"});

  // Ahead: the full chain, a newer queue_schema row and an extra column.
  auto const ahead = scratch.file("ahead.db");
  {
    auto conn = make_queue_db(ahead);
    REQUIRE(conn.execute("insert into queue_schema (version, compat, description) values (3, 2, 'future')").has_value());
    REQUIRE(conn.execute("alter table queue_history add column future_column text").has_value());
    insert_run(conn, 2000001, "T", R"(["later"])", "exited", 0, 10);
  }
  auto const new_runs = read_tagged_runs(ahead, "T");
  REQUIRE(new_runs.has_value());
  CHECK(new_runs->state == reader_state::present);
  REQUIRE(new_runs->runs.size() == 1);
  CHECK(new_runs->runs[0].argv == std::vector<std::string>{"later"});
}

TEST_CASE("reader: a quiet WAL database reads normally and leaves only owner-only sidecars", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("quiet.db");
  create_owner_only(path);
  {
    // connection::open puts the database in WAL mode. Closing the last connection checkpoints it and removes both sidecars.
    auto conn = make_queue_db(path);
    insert_run(conn, 2000001, "T", R"(["quiet"])", "exited", 0, 1);
  }
  auto const wal = std::filesystem::path{path.string() + "-wal"};
  auto const shm = std::filesystem::path{path.string() + "-shm"};
  REQUIRE_FALSE(std::filesystem::exists(wal));
  REQUIRE_FALSE(std::filesystem::exists(shm));
  auto const before = file_bytes(path);

  auto const runs = read_tagged_runs(path, "T");
  REQUIRE(runs.has_value());
  CHECK(runs->state == reader_state::present);
  REQUIRE(runs->runs.size() == 1);
  CHECK(runs->runs[0].argv == std::vector<std::string>{"quiet"});

  // Any sidecar the read created belongs to this user and is owner-only, as the database file is.
  for (auto const& sidecar : {wal, shm}) {
    if (!std::filesystem::exists(sidecar)) {
      continue;
    }
    struct stat info{};
    REQUIRE(::stat(sidecar.c_str(), &info) == 0);
    CHECK(info.st_uid == ::geteuid());
    CHECK((info.st_mode & 0777) == 0600);
  }
  CHECK(file_bytes(path) == before);

  auto conn = open_reader(path);
  REQUIRE(conn.has_value());
  auto write = conn->execute("insert into queue_settings (key, value) values ('slots', '4')");
  REQUIRE_FALSE(write.has_value());
  CHECK(planar::db::is_access_failure(write.error()));
}

TEST_CASE("reader: a database held by another connection past the bound reads as busy", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("locked.db");
  {
    auto conn = make_queue_db(path);
    insert_run(conn, 2000001, "T", R"(["held"])", "exited", 0, 1);
    // Rollback journal, so an exclusive lock blocks readers as well as writers.
    REQUIRE(conn.execute("pragma journal_mode = delete").has_value());
  }

  // open_existing, not open: `open` switches the journal to WAL, which would let readers past the lock.
  auto holder = planar::db::connection::open_existing(path.string(), 1000);
  REQUIRE(holder.has_value());
  REQUIRE(holder->execute("begin exclusive").has_value());

  auto const runs = read_tagged_runs(path, "T");
  REQUIRE_FALSE(runs.has_value());
  CHECK(runs.error().kind == reader_error_kind::busy);
  CHECK(runs.error().reason.find(path.string()) != std::string::npos);

  auto const facts = read_queue_facts(path);
  REQUIRE_FALSE(facts.has_value());
  CHECK(facts.error().kind == reader_error_kind::busy);

  REQUIRE(holder->execute("rollback").has_value());
}

TEST_CASE("reader: a file the reader cannot open reads as unreadable, not absent", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("denied.db");
  {
    auto conn = make_queue_db(path);
    insert_run(conn, 2000001, "T", R"(["denied"])", "exited", 0, 1);
  }
  restore_owner_mode restore{path};
  std::error_code    ec;
  std::filesystem::permissions(path, std::filesystem::perms::none, ec);
  REQUIRE_FALSE(ec);

  if (::geteuid() == 0) {
    // Root reads a mode-000 file, so the reader must read it rather than misreport it.
    auto const runs = read_tagged_runs(path, "T");
    REQUIRE(runs.has_value());
    CHECK(runs->state == reader_state::present);
    return;
  }

  auto const runs = read_tagged_runs(path, "T");
  REQUIRE_FALSE(runs.has_value());
  CHECK(runs.error().kind == reader_error_kind::unreadable);
  CHECK(runs.error().reason.find(path.string()) != std::string::npos);
  CHECK(read_queue_facts(path).error().kind == reader_error_kind::unreadable);
}

TEST_CASE("reader: a missing database is absent, with no runs, and nothing is created", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("never.db");

  auto const runs = read_tagged_runs(path, "T");
  REQUIRE(runs.has_value());
  CHECK(runs->state == reader_state::absent);
  CHECK_FALSE(runs->created_at.has_value());
  CHECK(runs->tagged_history_days == 90);
  CHECK(runs->runs.empty());

  auto const facts = read_queue_facts(path);
  REQUIRE(facts.has_value());
  CHECK(facts->state == reader_state::absent);
  CHECK_FALSE(facts->created_at.has_value());
  CHECK(facts->tagged_history_days == 90);

  CHECK_FALSE(std::filesystem::exists(path));
  CHECK_FALSE(std::filesystem::exists(std::filesystem::path{path.string() + "-wal"}));
  CHECK_FALSE(std::filesystem::exists(std::filesystem::path{path.string() + "-shm"}));
}

TEST_CASE("reader: a planar.db or a queue database without queue_history.tag is incompatible", "[buildqueue][reader]") {
  scratch_dir scratch;

  auto const main_db = scratch.file("planar.db");
  {
    auto conn = planar::db::connection::open(main_db.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
  }
  auto const main_runs = read_tagged_runs(main_db, "T");
  REQUIRE_FALSE(main_runs.has_value());
  CHECK(main_runs.error().kind == reader_error_kind::incompatible);
  CHECK(main_runs.error().reason.find("queue_identity") != std::string::npos);

  auto const no_tag = scratch.file("no-tag.db");
  {
    auto conn = make_queue_db(no_tag);
    REQUIRE(conn.execute("drop index idx_queue_history_tag").has_value());
    REQUIRE(conn.execute("alter table queue_history drop column tag").has_value());
  }
  auto const runs = read_tagged_runs(no_tag, "T");
  REQUIRE_FALSE(runs.has_value());
  CHECK(runs.error().kind == reader_error_kind::incompatible);
  CHECK(runs.error().reason.find("queue_history") != std::string::npos);
  CHECK(runs.error().reason.find("tag") != std::string::npos);

  // The header read needs no history columns, so the same file still yields its header.
  auto const facts = read_queue_facts(no_tag);
  REQUIRE(facts.has_value());
  CHECK(facts->state == reader_state::present);
}

TEST_CASE("reader: a stored argv that is not a JSON array of strings is unreadable", "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("bad-argv.db");
  {
    auto conn = make_queue_db(path);
    insert_run(conn, 2000001, "T", R"({"not":"an array"})", "exited", 0, 1);
  }
  auto const runs = read_tagged_runs(path, "T");
  REQUIRE_FALSE(runs.has_value());
  CHECK(runs.error().kind == reader_error_kind::unreadable);
  CHECK(runs.error().reason.find("argv") != std::string::npos);
}

TEST_CASE("reader: a stored argv with escaped characters decodes to the strings the engine's decoder yields",
          "[buildqueue][reader]") {
  scratch_dir scratch;
  auto const  path = scratch.file("escapes.db");
  {
    auto conn = make_queue_db(path);
    // A quote, a backslash, a solidus escape, a tab, a \u escape and literal UTF-8 (U+00E9 both ways).
    insert_run(conn, 2000001, "T", R"(["a\"b","back\\slash","\/","x\ty","é","é"])", "exited", 0, 1);
  }
  auto const runs = read_tagged_runs(path, "T");
  REQUIRE(runs.has_value());
  REQUIRE(runs->runs.size() == 1);
  CHECK(runs->runs[0].argv == std::vector<std::string>{"a\"b", "back\\slash", "/", "x\ty", "\xC3\xA9", "\xC3\xA9"});
}
