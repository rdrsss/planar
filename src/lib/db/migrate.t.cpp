// @file migrate.t.cpp
// @brief Unit tests for `planar.db.migrations` / `planar.db.migrate` (plan
// 996, tasks cpp-db-migrations-embed and cpp-db-roundtrip). Exercises the
// real #embed-generated chain against on-disk SQLite databases: full
// apply-to-head, the anti-glob-order strictly-monotonic-versions mandate
// (tech-spec § "Embedded migrations and templates"), idempotent
// re-application, a rollback-on-failure test seam that never touches real
// migration files, a row-for-row parity check against a Zig-migrated
// fixture database, and the down-direction losslessness sweep required by
// test-spec.md's "Edge — up/down/up roundtrip is lossless at every
// version" scenario: an up→down→up schema-dump comparison at every
// version in the chain, plus a strict-descending-mirror-order rollback
// sweep from head down to an empty database.
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

/// @brief A canonical, order-independent textual dump of every schema
/// object (tables, indexes, triggers, views — everything `sqlite_master`
/// tracks) currently present on `conn`. Sorted by `(type, name, tbl_name)`
/// so two databases built by different call sequences but with identical
/// resulting structure compare equal; each row's own DDL text (the `sql`
/// column) is included verbatim, so a losslessness check here also covers
/// column lists, CHECK constraints, and index/trigger definitions — not
/// just object names. Deliberately schema-only (no data rows): the
/// roundtrip contract under test is "down→up restores the same structure
/// the original up produced", not data preservation across a rebuild-style
/// migration (test-spec.md's roundtrip scenario is scoped to schema).
///
/// One normalization is applied: double-quote characters are stripped from
/// the `sql` text. This is load-bearing, not cosmetic-for-convenience —
/// verified empirically (task cpp-db-roundtrip discovery) that SQLite's own
/// `ALTER TABLE ... RENAME TO` rewrites a table's stored `CREATE TABLE`
/// text to wrap the bare identifier in double quotes (`CREATE TABLE
/// "tasks" (` vs `CREATE TABLE tasks (`), while a plain `ALTER TABLE ADD
/// COLUMN` on a never-renamed table does not — an inconsistency confirmed
/// to be internal SQLite stringification only: `PRAGMA table_info`,
/// `PRAGMA foreign_key_list`, and `PRAGMA index_list` are byte-identical
/// between the two forms for every affected table (decisions, questions,
/// tasks, test_scenarios — the migration 00011 down script's
/// create+copy+drop+rename rebuild targets). None of our migrations ever
/// author a double-quoted identifier or a double-quoted string literal in
/// DDL (the project's `.sqlfluff` dialect is bare lowercase identifiers,
/// single-quoted literals), so stripping `"` cannot mask a real
/// column/constraint/index difference — it only removes SQLite's own
/// non-deterministic requoting artifact from the comparison.
std::string canonical_schema_dump(planar::db::connection& conn) {
  auto stmt = conn.prepare("select type, name, tbl_name, replace(ifnull(sql, ''), '\"', '') "
                           "from sqlite_master order by type, name, tbl_name");
  REQUIRE(stmt.has_value());

  std::string out;
  for (;;) {
    auto step = stmt->step();
    REQUIRE(step.has_value());
    if (*step == planar::db::step_result::done) {
      break;
    }
    // Unit-separator-delimited fields / record-separator-delimited rows —
    // arbitrary but fixed control bytes that cannot appear in SQL text or
    // sqlite_master identifiers, so no field/row can be misread as another.
    out += stmt->column_text(0);
    out += '\x1f';
    out += stmt->column_text(1);
    out += '\x1f';
    out += stmt->column_text(2);
    out += '\x1f';
    out += stmt->column_text(3);
    out += '\x1e';
  }
  return out;
}

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

TEST_CASE("apply_all takes the write lock synchronously at BEGIN (lock_mode::immediate), so a "
          "concurrent migrator loses cleanly before any migration DDL runs",
          "[db][migrate][lock-mode]") {
  // M1 boundary-review finding R1: two Planar binaries can both start
  // against the same on-disk database and both try to migrate it. This
  // pins the fix by simulating exactly that race with two real
  // connections to the same file: connection A stands in for a
  // concurrent migrator that already won the race and is holding its
  // migration transaction open; connection B then calls the real
  // `apply_all` and must fail right at `begin_transaction` (SQLITE_BUSY,
  // default busy_timeout is 0) rather than partway through executing a
  // migration script.
  constexpr int k_sqlite_busy = 5; // SQLITE_BUSY

  scratch_db_path scratch;

  auto conn_a = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_a.has_value());
  auto conn_b = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_b.has_value());

  {
    // Stand-in for a concurrent migrator that has already reached its
    // per-migration `begin_transaction(lock_mode::immediate)` call and is
    // holding the write lock while its migration script runs.
    auto txn_a = conn_a->begin_transaction(planar::db::lock_mode::immediate);
    REQUIRE(txn_a.has_value());

    // `current_version` is a plain read (`prepare`/`step`, no explicit
    // transaction) so it is unaffected by A's RESERVED lock and correctly
    // reports a fresh database (0) — the failure below happens at
    // apply_all's own `begin_transaction(lock_mode::immediate)` call for
    // the first pending migration, not at the version read.
    auto result = planar::db::apply_all(*conn_b);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code_ == k_sqlite_busy);

    // Confirms the failure was clean: nothing from the first migration's
    // DDL landed on B's side (B never got that far), and A's own
    // in-progress migration state (nothing committed yet either) is
    // untouched.
    auto stmt = conn_b->prepare("select count(*) from sqlite_master where type = 'table'");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().value() == planar::db::step_result::row);
    REQUIRE(stmt->column_int64(0) == 0);

    // `txn_a` goes out of scope here without a commit — rollback releases
    // the lock, standing in for the "winner" finishing its migration.
  }

  // With the lock released, a normal apply_all now succeeds and reaches
  // head — confirms the earlier failure was genuinely the lock race, not
  // a side effect that left the database or connection unusable.
  REQUIRE(planar::db::apply_all(*conn_b));
  auto stmt = conn_b->prepare("select count(*), max(version) from schema_migrations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == stmt->column_int64(1));
  REQUIRE(stmt->column_int64(1) == 33);
}

TEST_CASE("parity: a C++-migrated database matches a Zig-migrated database row-for-row", "[db][migrate][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }

  scratch_db_path zig_scratch;
  // Run from a NEUTRAL directory, not ctest's cwd.
  //
  // `init` is a planning verb, and the worktree gate refuses planning verbs
  // whose cwd sits inside a linked git worktree (exit 8). The orchestrator
  // dispatches coders into `.claude/worktrees/`, so ctest's cwd is inside one
  // for every isolated cycle and this test failed there while passing in the
  // main checkout — a deterministic red that looks unrelated to whatever the
  // cycle changed (task 6144).
  //
  // `--skip-project` means the invocation registers no project and therefore
  // has no use for the cwd at all, so relocating is honest rather than a
  // bypass: the gate stays fully armed, and this test simply stops standing
  // where it fires. Deliberately NOT solved with
  // `PLANAR_DISABLE_WORKTREE_GATE` — that escape hatch is compiled into the
  // C++ debug build only, and this line shells the ZIG reference binary.
  //
  // The `cd <dir> && env VAR=...` form is required: in this platform's
  // /bin/sh, assignments that PRECEDE a `cd` do not survive the `&&`, so the
  // older `PLANAR_DB=... cd x && bin` shape silently loses the variable and
  // sends the reference binary at the operator's real database.
  const auto neutral = std::filesystem::temp_directory_path().string();
  const auto cmd = std::format("cd {} && env PLANAR_DB={} {} init --skip-project --allow-no-repo --json >/dev/null 2>&1", neutral,
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

TEST_CASE("up-down-up roundtrip is lossless at every version in the chain", "[db][migrate][roundtrip]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 33);

  // Walk the chain forward one migration at a time. At each version,
  // capture the schema, roll that single migration back, re-apply it, and
  // require the schema to come back byte-identical. This is a per-version
  // loop (not just a check at the tip) by construction: every iteration
  // exercises a distinct migration's down/up pair, and the loop only ever
  // holds exactly the versions 1..i applied — the same precondition each
  // migration's own down script was written against — so a migration whose
  // down leaves stray/missing state is caught either as an apply_all
  // failure (second up_sql run hits e.g. "table already exists") or as a
  // dump mismatch, never silently.
  for (std::size_t i = 0; i < chain.size(); ++i) {
    REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, i + 1)));

    const auto before = canonical_schema_dump(*conn);

    REQUIRE(conn->execute(chain[i].down_sql_));
    REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, i + 1)));

    const auto after = canonical_schema_dump(*conn);

    INFO(std::format("migration version {} ('{}') is not lossless across down/up", chain[i].version_, chain[i].name_));
    REQUIRE(before == after);
  }

  // The sweep is constructive: it leaves the database fully migrated to
  // head, so this also re-confirms the "fresh-DB migrate to head" contract
  // held throughout every intermediate down/up step.
  auto stmt = conn->prepare("select count(*), max(version) from schema_migrations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  REQUIRE(stmt->column_int64(0) == stmt->column_int64(1));
  REQUIRE(stmt->column_int64(1) == 33);
}

TEST_CASE("the down chain from head deletes schema_migrations rows in strict descending mirror order, "
          "leaving no orphan rows, and a full rollback reaches an empty database",
          "[db][migrate][rollback]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 33);

  // Roll back one migration at a time, from the tip down to the
  // foundation, asserting the mirror-order contract at every step: before
  // rolling back migration i, schema_migrations' highest row must be
  // exactly version i (nothing skipped ahead of it); after rolling it
  // back, the highest row must be exactly version i-1 (nothing left
  // behind as an orphan) — except at the foundation migration (i == 0),
  // whose down drops the schema_migrations table itself.
  for (std::size_t idx = chain.size(); idx-- > 0;) {
    const auto& m = chain[idx];

    {
      auto before = conn->prepare("select max(version) from schema_migrations");
      REQUIRE(before.has_value());
      REQUIRE(before->step().value() == planar::db::step_result::row);
      REQUIRE(static_cast<std::uint32_t>(before->column_int64(0)) == m.version_);
    }

    REQUIRE(conn->execute(m.down_sql_));

    if (idx == 0) {
      auto after = conn->prepare("select max(version) from schema_migrations");
      REQUIRE_FALSE(after.has_value()); // table itself was dropped
    } else {
      auto after = conn->prepare("select max(version) from schema_migrations");
      REQUIRE(after.has_value());
      REQUIRE(after->step().value() == planar::db::step_result::row);
      REQUIRE(static_cast<std::uint32_t>(after->column_int64(0)) == chain[idx - 1].version_);
    }
  }

  // Full rollback to zero: no Planar-authored schema object of any kind
  // survives. `sqlite_sequence` is excluded deliberately: SQLite creates it
  // itself, automatically, the first time any `integer primary key
  // autoincrement` table is created (foundation's own `projects` table
  // qualifies), and never removes it just because the autoincrement tables
  // that triggered its creation were later dropped (verified empirically —
  // it survives a full down-chain rollback to zero regardless of migration
  // content). It is SQLite's own bookkeeping object, not something any
  // down script is expected to manage, and not an orphan of our schema.
  auto leftover = conn->prepare("select count(*) from sqlite_master where name != 'sqlite_sequence'");
  REQUIRE(leftover.has_value());
  REQUIRE(leftover->step().value() == planar::db::step_result::row);
  REQUIRE(leftover->column_int64(0) == 0);
}
