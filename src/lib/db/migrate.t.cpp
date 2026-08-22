// @file migrate.t.cpp
// @brief Unit tests for `planar.db.migrations` / `planar.db.migrate` (plan
// 996, task cpp-db-migrations-embed). Exercises the real #embed-generated
// chain against on-disk SQLite databases: full apply-to-head, the
// anti-glob-order strictly-monotonic-versions mandate (tech-spec §
// "Embedded migrations and templates"), idempotent re-application, a
// rollback-on-failure test seam that never touches real migration files,
// and a row-for-row parity check against a Zig-migrated fixture database.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp):
// MSVC's supported direction for mixing textual std headers with IFC
// imports is include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrations;
import planar.db.migrate;

namespace {

/// @brief A unique scratch database path under the system temp directory,
/// removed (best-effort, including SQLite's `-journal`/`-wal`/`-shm`
/// sidecars) when the guard goes out of scope. Mirrors db.t.cpp's helper —
/// duplicated rather than shared because this codebase has no header tree
/// for first-party code (modules-only).
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_migrate_test_{}_{}.db",
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

TEST_CASE("apply_all migrates a fresh database to head", "[db][migrate]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  REQUIRE(planar::db::apply_all(*conn));

  auto stmt = conn->prepare("select count(*), max(version) from schema_migrations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == stmt->column_int64(1)); // count == max version: no gaps
  REQUIRE(stmt->column_int64(1) == 33);
}

TEST_CASE("the embedded migration chain's versions are strictly monotonic", "[db][migrations]") {
  auto chain = planar::db::migrations();
  REQUIRE(chain.size() > 1);
  for (std::size_t i = 1; i < chain.size(); ++i) {
    REQUIRE(chain[i].version_ > chain[i - 1].version_);
  }
}

TEST_CASE("applying to an already-current database is a no-op", "[db][migrate]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  REQUIRE(planar::db::apply_all(*conn));
  // Second call must NOT re-run any migration (would fail with "table
  // already exists" otherwise).
  REQUIRE(planar::db::apply_all(*conn));

  auto stmt = conn->prepare("select count(*) from schema_migrations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == static_cast<std::int64_t>(planar::db::migrations().size()));
}

TEST_CASE("a failing migration rolls back its own transaction, leaving schema_migrations unchanged",
          "[db][migrate][error-path]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  // Bring the database to head first, using the real embedded chain.
  REQUIRE(planar::db::apply_all(*conn));

  auto before = conn->prepare("select count(*), max(version) from schema_migrations");
  REQUIRE(before.has_value());
  REQUIRE(before->step().value() == planar::db::step_result::row);
  const auto count_before = before->column_int64(0);
  const auto max_before   = before->column_int64(1);

  // Deliberately-broken synthetic next migration, injected via the
  // apply_all(conn, chain) test seam — never edit real migration files
  // (immutable inputs).
  const planar::db::migration_record broken{
      .version_  = static_cast<std::uint32_t>(max_before + 1),
      .name_     = "broken",
      .up_sql_   = "this is not valid sql at all;",
      .down_sql_ = "",
  };
  const std::array<planar::db::migration_record, 1> broken_chain{broken};

  auto result = planar::db::apply_all(*conn, broken_chain);
  REQUIRE_FALSE(result.has_value());

  auto after = conn->prepare("select count(*), max(version) from schema_migrations");
  REQUIRE(after.has_value());
  REQUIRE(after->step().value() == planar::db::step_result::row);
  REQUIRE(after->column_int64(0) == count_before);
  REQUIRE(after->column_int64(1) == max_before);
}

TEST_CASE("parity: a C++-migrated database matches a Zig-migrated database row-for-row", "[db][migrate][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }

  scratch_db_path zig_scratch;
  const auto      cmd = std::format("PLANAR_DB={} {} init --skip-project --allow-no-repo --json >/dev/null 2>&1",
                                    zig_scratch.path_.string(), zig_bin.string());
  REQUIRE(std::system(cmd.c_str()) == 0);

  auto zig_conn = planar::db::connection::open_read_only(zig_scratch.path_.string());
  REQUIRE(zig_conn.has_value());

  scratch_db_path cpp_scratch;
  auto            cpp_conn = planar::db::connection::open(cpp_scratch.path_.string());
  REQUIRE(cpp_conn.has_value());
  REQUIRE(planar::db::apply_all(*cpp_conn));

  auto zig_stmt = zig_conn->prepare("select version, description from schema_migrations order by version");
  auto cpp_stmt = cpp_conn->prepare("select version, description from schema_migrations order by version");
  REQUIRE(zig_stmt.has_value());
  REQUIRE(cpp_stmt.has_value());

  std::size_t rows = 0;
  for (;;) {
    auto zig_step = zig_stmt->step();
    auto cpp_step = cpp_stmt->step();
    REQUIRE(zig_step.has_value());
    REQUIRE(cpp_step.has_value());
    REQUIRE(*zig_step == *cpp_step);
    if (*zig_step == planar::db::step_result::done) {
      break;
    }
    REQUIRE(zig_stmt->column_int64(0) == cpp_stmt->column_int64(0));
    REQUIRE(zig_stmt->column_text(1) == cpp_stmt->column_text(1));
    ++rows;
  }
  REQUIRE(rows == 33);
}
