// @file db.t.cpp
// @brief Unit tests for `planar.db` (plan 996, task cpp-db-wrapper).
// Exercises the module's real export surface against real on-disk SQLite
// databases under a per-test scratch file: successful open, read-only-open
// failure on a missing path, a typed error for invalid SQL (sqlite result
// code preserved), a rejected foreign-key violation, transaction
// commit-persists / rollback-on-scope-exit-discards, and a read-only
// connection refusing a write.
//
// Include-before-import is deliberate (see core/version.t.cpp): MSVC's
// supported direction for mixing textual std headers with IFC imports is
// include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;

namespace {

// Well-known SQLite (extended) result codes, mirrored here rather than
// pulling in <sqlite3.h> from a test translation unit — planar.db keeps the
// raw C API confined to its own global module fragment (db.cppm/db.cpp),
// and the numeric values are part of SQLite's stable public ABI.
constexpr int k_sqlite_error                 = 1;   // SQLITE_ERROR
constexpr int k_sqlite_readonly              = 8;   // SQLITE_READONLY
constexpr int k_sqlite_cantopen              = 14;  // SQLITE_CANTOPEN
constexpr int k_sqlite_constraint_foreignkey = 787; // SQLITE_CONSTRAINT_FOREIGNKEY

/// @brief A unique scratch database path under the system temp directory,
/// removed (best-effort, including SQLite's `-journal`/`-wal`/`-shm`
/// sidecars) when the guard goes out of scope.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_db_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

} // namespace

TEST_CASE("connection::open succeeds against a fresh path and can round-trip data", "[db][connection]") {
  scratch_db_path scratch;

  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE_FALSE(conn->is_read_only());

  REQUIRE(conn->execute("create table t (n integer);"));
  REQUIRE(conn->execute("insert into t (n) values (42);"));

  auto stmt = conn->prepare("select n from t;");
  REQUIRE(stmt.has_value());
  auto step1 = stmt->step();
  REQUIRE(step1.has_value());
  REQUIRE(*step1 == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == 42);
}

TEST_CASE("connection::open_read_only fails on a nonexistent path", "[db][connection][error-path]") {
  scratch_db_path scratch; // never created — path simply does not exist.

  auto conn = planar::db::connection::open_read_only(scratch.path_.string());
  REQUIRE_FALSE(conn.has_value());
  REQUIRE(conn.error().code_ == k_sqlite_cantopen);
  REQUIRE_FALSE(conn.error().message_.empty());
}

TEST_CASE("invalid SQL yields a typed error with the sqlite result code preserved", "[db][statement][error-path]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  auto stmt = conn->prepare("select this is not valid sql;");
  REQUIRE_FALSE(stmt.has_value());
  REQUIRE(stmt.error().code_ == k_sqlite_error);
  REQUIRE_FALSE(stmt.error().message_.empty());
}

TEST_CASE("a foreign-key violation is rejected", "[db][connection][error-path]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  REQUIRE(conn->execute("create table parent (id integer primary key);"));
  REQUIRE(conn->execute("create table child (id integer primary key, parent_id integer references parent(id));"));

  auto result = conn->execute("insert into child (id, parent_id) values (1, 999);");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code_ == k_sqlite_constraint_foreignkey);
}

TEST_CASE("transaction commit persists the write", "[db][transaction]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (n integer);"));

  {
    auto txn = conn->begin_transaction();
    REQUIRE(txn.has_value());
    REQUIRE(conn->execute("insert into t (n) values (1);"));
    REQUIRE(txn->commit());
  }

  auto stmt = conn->prepare("select count(*) from t;");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == 1);
}

TEST_CASE("transaction rollback-on-scope-exit discards the write", "[db][transaction]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (n integer);"));

  {
    auto txn = conn->begin_transaction();
    REQUIRE(txn.has_value());
    REQUIRE(conn->execute("insert into t (n) values (1);"));
    // Deliberately no commit() — the transaction goes out of scope here and
    // must roll back.
  }

  auto stmt = conn->prepare("select count(*) from t;");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == 0);
}

TEST_CASE("a read-only connection refuses a write", "[db][connection][error-path]") {
  scratch_db_path scratch;

  // Phase 1: seed via a writable handle, then let it close (RAII) before
  // opening the read-only handle.
  {
    auto seed = planar::db::connection::open(scratch.path_.string());
    REQUIRE(seed.has_value());
    REQUIRE(seed->execute("create table t (n integer); insert into t (n) values (1);"));
  }

  auto conn = planar::db::connection::open_read_only(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->is_read_only());

  auto result = conn->execute("insert into t (n) values (2);");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code_ == k_sqlite_readonly);

  // Reads still work, and the rejected write left no trace.
  auto stmt = conn->prepare("select count(*) from t;");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == 1);
}
