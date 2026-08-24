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
constexpr int k_sqlite_busy                  = 5;   // SQLITE_BUSY
constexpr int k_sqlite_readonly              = 8;   // SQLITE_READONLY
constexpr int k_sqlite_cantopen              = 14;  // SQLITE_CANTOPEN
constexpr int k_sqlite_misuse                = 21;  // SQLITE_MISUSE
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

TEST_CASE("begin_transaction(lock_mode::immediate) takes the write lock synchronously at BEGIN, "
          "blocking a concurrent writer before any statement runs",
          "[db][transaction][lock-mode]") {
  scratch_db_path scratch;
  auto            seed = planar::db::connection::open(scratch.path_.string());
  REQUIRE(seed.has_value());
  REQUIRE(seed->execute("create table t (n integer);"));

  auto conn_a = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_a.has_value());
  auto conn_b = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_b.has_value());

  {
    auto txn_a = conn_a->begin_transaction(planar::db::lock_mode::immediate);
    REQUIRE(txn_a.has_value());

    // `txn_a` has not executed a single statement yet — under `deferred`
    // mode this would still be lock-free. Because it requested
    // `immediate`, the RESERVED (write) lock was already taken
    // synchronously inside `begin_transaction` above, so a second
    // connection's plain write fails right now with SQLITE_BUSY (default
    // busy_timeout is 0 — no retry window) rather than succeeding or
    // blocking indefinitely.
    auto blocked_write = conn_b->execute("insert into t (n) values (1);");
    REQUIRE_FALSE(blocked_write.has_value());
    REQUIRE(blocked_write.error().code_ == k_sqlite_busy);

    // `txn_a` goes out of scope here without a commit — rollback-on-
    // scope-exit releases the RESERVED lock.
  }

  // Confirms the earlier block was genuinely the RESERVED lock (now
  // released), not some permanent failure.
  REQUIRE(conn_b->execute("insert into t (n) values (2);"));
}

TEST_CASE("begin_transaction() default (deferred) takes no lock at BEGIN, so a concurrent writer "
          "is not blocked until this transaction itself performs a write",
          "[db][transaction][lock-mode]") {
  scratch_db_path scratch;
  auto            seed = planar::db::connection::open(scratch.path_.string());
  REQUIRE(seed.has_value());
  REQUIRE(seed->execute("create table t (n integer);"));

  auto conn_a = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_a.has_value());
  auto conn_b = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_b.has_value());

  auto txn_a = conn_a->begin_transaction(); // default: lock_mode::deferred
  REQUIRE(txn_a.has_value());

  // Contrast case for the test above: with the default deferred mode,
  // `begin_transaction` alone takes no lock, so a second connection's
  // write succeeds immediately.
  REQUIRE(conn_b->execute("insert into t (n) values (1);"));
}

TEST_CASE("commit() on a moved-from transaction returns a guarded error instead of crashing", "[db][transaction][error-path]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (n integer);"));

  auto txn = conn->begin_transaction();
  REQUIRE(txn.has_value());

  // Move `*txn`'s transaction elsewhere — `txn`'s own transaction object
  // is now moved-from (its `_handle` is null; see the move constructor).
  planar::db::transaction moved_into = std::move(*txn);

  auto result = txn->commit();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code_ == k_sqlite_misuse);
  // `code_` alone doesn't discriminate: an UNGUARDED `sqlite3_exec(nullptr,
  // ...)` also returns SQLITE_MISUSE (`sqlite3SafetyCheckOk` is checked
  // unconditionally at that call site, independent of
  // `SQLITE_ENABLE_API_ARMOR`), so the code matches with or without the
  // guard. Pin the guard's own message so this actually fails if the guard
  // is removed.
  REQUIRE(result.error().message_ == "planar.db: commit() on a moved-from transaction");

  // `moved_into` still owns the live transaction and rolls it back cleanly
  // on scope exit. The guard above exists to document this contract
  // explicitly and as defense-in-depth against a future vendor bump — this
  // module's vendored SQLite already self-guards a null `sqlite3*` inside
  // `sqlite3_exec`/`sqlite3_errmsg`, so it would not have crashed even
  // without the guard (see db.cppm's `commit()` doc comment).
}

TEST_CASE("a second commit() on an already-committed transaction returns a guarded error instead of "
          "re-issuing COMMIT",
          "[db][transaction][error-path]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (n integer);"));

  auto txn = conn->begin_transaction();
  REQUIRE(txn.has_value());
  REQUIRE(conn->execute("insert into t (n) values (1);"));
  REQUIRE(txn->commit());

  // A second, unrelated transaction is now open on the same connection —
  // if the guard were missing, a stray second `commit()` below would
  // re-issue `COMMIT` and land on THIS transaction instead of erroring.
  auto other_txn = conn->begin_transaction();
  REQUIRE(other_txn.has_value());
  REQUIRE(conn->execute("insert into t (n) values (2);"));

  auto second_commit = txn->commit();
  REQUIRE_FALSE(second_commit.has_value());
  REQUIRE(second_commit.error().code_ == k_sqlite_misuse);

  // The unrelated transaction was never touched by the stray commit
  // attempt above — it is still open and rolls back on scope exit here.
  REQUIRE(conn->execute("rollback;"));

  auto stmt = conn->prepare("select count(*) from t;");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == 1); // only the first, genuinely-committed insert persisted
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

// --- bind_text null-data normalisation (plan 996, task 6097) ---------------
//
// `sqlite3_bind_text(stmt, i, nullptr, 0, ...)` binds SQL NULL, and a
// default-constructed `std::string_view` has a null `data()`. Callers cannot
// tell the two empty views apart (`.empty()` is true for both), so binding
// them differently is a silent-corruption trap: it drove 41 annotation tests
// red at task 6094 and was papered over with a per-module `nn()` guard in six
// engine buckets. These tests pin the root contract instead.
//
// Every assertion below reads `typeof(...)` out of SQLite rather than the
// value, because that is the ONLY way to distinguish stored NULL from a
// stored empty string -- comparing `column_text(...)` against `""` passes for
// both and is exactly the assertion that let the original bug through.

TEST_CASE("bind_text binds '' -- not SQL NULL -- for a default-constructed string_view", "[db][statement][bind_text][6097]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (id integer primary key, v text);"));

  // The trap input: `.data()` really is null, so this is not a vacuous test.
  std::string_view const defaulted{};
  REQUIRE(defaulted.data() == nullptr);
  REQUIRE(defaulted.empty());

  auto stmt = conn->prepare("insert into t (id, v) values (1, ?);");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, defaulted));
  REQUIRE(stmt->step().value() == planar::db::step_result::done);

  auto read = conn->prepare("select typeof(v), v, v is null from t where id = 1;");
  REQUIRE(read.has_value());
  REQUIRE(read->step().value() == planar::db::step_result::row);
  REQUIRE(read->column_text(0) == "text"); // would be "null" before the fix
  REQUIRE(read->column_text(1).empty());
  REQUIRE(read->column_int64(2) == 0);
}

TEST_CASE("bind_text is indistinguishable for null-data and non-null-data empty views", "[db][statement][bind_text][6097]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (id integer primary key, v text);"));

  std::string_view const defaulted{};
  std::string_view const from_literal{""};
  // The two inputs differ in exactly one respect that no caller can observe.
  REQUIRE(defaulted.data() == nullptr);
  REQUIRE(from_literal.data() != nullptr);
  REQUIRE(defaulted.empty());
  REQUIRE(from_literal.empty());

  auto ins = conn->prepare("insert into t (id, v) values (?, ?);");
  REQUIRE(ins.has_value());
  REQUIRE(ins->bind_int64(1, 1));
  REQUIRE(ins->bind_text(2, defaulted));
  REQUIRE(ins->step().value() == planar::db::step_result::done);
  REQUIRE(ins->reset());
  REQUIRE(ins->bind_int64(1, 2));
  REQUIRE(ins->bind_text(2, from_literal));
  REQUIRE(ins->step().value() == planar::db::step_result::done);

  // Same storage class, same value, for both rows.
  auto read = conn->prepare("select typeof(v) from t order by id;");
  REQUIRE(read.has_value());
  REQUIRE(read->step().value() == planar::db::step_result::row);
  REQUIRE(read->column_text(0) == "text");
  REQUIRE(read->step().value() == planar::db::step_result::row);
  REQUIRE(read->column_text(0) == "text");

  auto distinct = conn->prepare("select count(distinct typeof(v)) from t;");
  REQUIRE(distinct.has_value());
  REQUIRE(distinct->step().value() == planar::db::step_result::row);
  REQUIRE(distinct->column_int64(0) == 1);
}

TEST_CASE("bind_text satisfies a NOT NULL column for a default-constructed string_view", "[db][statement][bind_text][6097]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (id integer primary key, v text not null);"));

  std::string_view const defaulted{};
  REQUIRE(defaulted.data() == nullptr);

  // This is the exact shape that failed 41 annotation tests before `nn()`.
  auto stmt = conn->prepare("insert into t (id, v) values (1, ?);");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, defaulted));
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::done);
}

TEST_CASE("bind_null still binds SQL NULL, so callers keep an explicit way to ask for it", "[db][statement][bind_text][6097]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (id integer primary key, v text);"));

  auto stmt = conn->prepare("insert into t (id, v) values (1, ?);");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_null(1));
  REQUIRE(stmt->step().value() == planar::db::step_result::done);

  auto read = conn->prepare("select typeof(v), v is null from t where id = 1;");
  REQUIRE(read.has_value());
  REQUIRE(read->step().value() == planar::db::step_result::row);
  REQUIRE(read->column_text(0) == "null");
  REQUIRE(read->column_int64(1) == 1);
}

TEST_CASE("bind_text preserves embedded NUL bytes and does not stop at one", "[db][statement][bind_text][6097]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("create table t (id integer primary key, v text);"));

  // The fix substitutes a pointer, never a length, so the explicit size
  // still governs. A regression to a NUL-terminated bind would truncate to 1.
  std::string const      embedded("a\0b", 3);
  std::string_view const value{embedded};
  REQUIRE(value.size() == 3);

  auto stmt = conn->prepare("insert into t (id, v) values (1, ?);");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, value));
  REQUIRE(stmt->step().value() == planar::db::step_result::done);

  // `length(v)` on a TEXT value counts characters BEFORE the first NUL (it
  // would answer 1 here regardless of what was stored), so the byte count
  // has to be taken through a blob cast for this assertion to discriminate.
  auto read = conn->prepare("select length(cast(v as blob)), typeof(v) from t where id = 1;");
  REQUIRE(read.has_value());
  REQUIRE(read->step().value() == planar::db::step_result::row);
  REQUIRE(read->column_int64(0) == 3);
  REQUIRE(read->column_text(1) == "text");
}
