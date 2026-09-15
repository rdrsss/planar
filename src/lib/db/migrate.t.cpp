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
  REQUIRE(stmt->column_int64(1) == 36);
}

TEST_CASE("the embedded migration chain's versions are strictly monotonic", "[db][migrations]") {
  auto chain = planar::db::migrations();
  REQUIRE(chain.size() > 1);
  for (std::size_t i = 1; i < chain.size(); ++i) {
    REQUIRE(chain[i].version_ > chain[i - 1].version_);
  }
}

namespace {

/// @brief A single SQL text with `--`-to-end-of-line comments removed.
///
/// This project's migrations use only `--` line comments (`.sqlfluff`'s
/// dialect is plain lowercase SQLite SQL; no `/* */` block comments appear
/// anywhere in `migrations/`), so that is the only form this strips. A `--`
/// inside a single-quoted string literal would be misread as a comment
/// start, but no migration's string literals contain one — this is a test
/// helper scoped to the invariant below, not a general SQL comment stripper.
/// @param sql The full script text.
/// @return The text with every `-- ...` run to end-of-line removed.
auto strip_line_comments(std::string_view sql) -> std::string {
  std::string out;
  out.reserve(sql.size());
  std::size_t pos = 0;
  while (pos < sql.size()) {
    auto const comment = sql.find("--", pos);
    auto const eol     = sql.find('\n', pos);
    if (comment != std::string_view::npos && (eol == std::string_view::npos || comment < eol)) {
      out.append(sql.substr(pos, comment - pos));
      pos = sql.find('\n', comment);
      if (pos == std::string_view::npos) {
        break;
      }
    } else if (eol != std::string_view::npos) {
      out.append(sql.substr(pos, eol - pos + 1));
      pos = eol + 1;
    } else {
      out.append(sql.substr(pos));
      break;
    }
  }
  return out;
}

} // namespace

TEST_CASE("no migration authors a double-quoted identifier or literal", "[db][migrations][6056]") {
  // `canonical_schema_dump` (this file, above) strips every `"` from
  // `sqlite_master.sql` before comparing schemas, because SQLite's own
  // `ALTER TABLE ... RENAME TO` rewrites a table's stored `CREATE TABLE`
  // text to double-quote the identifier while a plain `ADD COLUMN` never
  // does -- pure engine stringification, not a real structural difference
  // (task 6056; migration 00011's down script is what triggers it). That
  // strip is safe ONLY because no migration's AUTHORED text contains a `"`
  // at all: if one ever did (a quoted identifier, or a double-quoted string
  // literal -- SQLite accepts both, ambiguously, depending on context), the
  // strip would silently erase real schema content instead of only an
  // artifact, and the roundtrip test could pass while masking an actual
  // loss.
  //
  // This is the enforcement task 6056 asked for: the invariant the strip
  // depends on, checked directly against the EMBEDDED migration text (what
  // actually ships), not the source files on disk -- so it fails the moment
  // a future migration violates it, at the same `ctest` gate everything
  // else in this file runs under.
  for (auto const& record : planar::db::migrations()) {
    INFO("migration " << record.version_ << " (" << record.name_ << ")");
    CHECK(strip_line_comments(record.up_sql_).find('"') == std::string::npos);
    CHECK(strip_line_comments(record.down_sql_).find('"') == std::string::npos);
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
  REQUIRE(stmt->column_int64(1) == 36);
}

TEST_CASE("parity: a C++-migrated database matches a Zig-migrated database row-for-row", "[db][migrate][parity]") {
  // Retired off the Zig oracle by task 6541 (plan 996, decision 963/982's
  // third gating condition: every oracle-conditional parity skip must be
  // gone before `zig/` can be deleted). This case used to shell the Zig
  // reference binary's `init` verb into a scratch database and compare its
  // `schema_migrations` rows against a C++-migrated database's, live, at
  // test time.
  //
  // `schema_migrations` is the public schema-version contract (see
  // `docs/architecture.md` § schema contract and the Migrations checklist
  // in CLAUDE.md): every migration's up script ends with exactly one
  // `insert into schema_migrations (version, description) values (...)`,
  // so the full (version, description) table below is not a derived
  // artifact of either runtime — it is the literal content of
  // `migrations/*.up.sql`, transcribed by reading those 35 files directly
  // (not paraphrased from this test's prior behaviour), which is also
  // exactly what the Zig oracle's own `init` used to embed and what this
  // test confirmed row-for-row before this retirement. Pinning it here
  // means the case still asserts the real contract — "the embedded C++
  // migration chain produces the schema-version row set the project has
  // actually authored" — without needing a second runtime to compare
  // against. Runs without the oracle: an assertion about this binary, not
  // a comparison.
  static const std::vector<std::pair<std::int64_t, std::string>> k_expected{
      {1, "initial schema"},
      {2, "planning entities: plans, artifacts, decisions"},
      {3, "work items: agents, tasks, questions, test_scenarios, plan_steps, active_scope"},
      {4, "entity_links: typed cross-entity relationships"},
      {5, "sessions, session_entries, context_snapshots, handoffs"},
      {6, "external plane: external_systems, external_links, sync_events"},
      {7, "workbench_sync_state: bidirectional workbench sync tracking"},
      {8, "artifacts.kind: add research, getting_started, changelog_entry, glossary_term"},
      {9, "drop active_scope table — cwd-primary scope resolution (plan 153)"},
      {10, "task_reopens audit table — done/cancelled escape hatch (plan 215 M1)"},
      {11, "slug refs + FTS5 search"},
      {12, "annotations: line-anchored notes with FTS5 indexing and entity_links widening"},
      {13, "artifacts.kind: add test_spec"},
      {14, "audit_log: append-only data-plane mutation trail"},
      {15, "agent activity tracking: agent_work_claims + agent_actions (with locality columns)"},
      {16, "agent_actions metadata: nullable JSON-shaped text column for caller-attached per-action context"},
      {17, "handoffs: add nullable worktree_path / repo_root / branch for cold-start resumer recovery"},
      {18, "correct schema_migrations descriptions for versions 2-7"},
      {19, "task_touch_paths: path-level task-touch declarations for the parallelizability rules"},
      {20, "opt-in cli_invocations usage log"},
      {21, "session_commits table plus sessions repo_root/head_sha_at_start for session commit attribution"},
      {22, "workflow context plane: workflow_runs + context_records tables"},
      {23, "agent_work_claims: nullable run_id (FK workflow_runs) + stage for planar-execute correlation"},
      {24, "context_records: make claim_id nullable for run-keyed capsule writes (decision 456)"},
      {25, "runs/run_events/run_touches: measurement-rig substrate for the decomposition experiment"},
      {26, "closures: derived symbol-level closure snapshot per task (M2 extractor)"},
      {27, "external sync per-field baseline for conflict detection"},
      {28, "structured feedback triage for task and question findings"},
      {29, "agent claim terminal failure categories"},
      {30, "adaptive routing registry, facts, dispatch, experiments, and evidence"},
      {31, "dispatch confirmation tokens"},
      {32, "optional comparable latency and cost metrics on terminal samples"},
      {33, "rename the blocks relationship to depends-on"},
      {34, "annotations: entity anchors, revisions, source identity, and operation receipts"},
      {35, "annotations: durable bulk receipt affected count"},
      {36, "cli_invocations: durable retryable busy category"},
  };
  REQUIRE(k_expected.size() == 36);

  scratch_db_path cpp_scratch;
  auto            cpp_conn = planar::db::connection::open(cpp_scratch.path_.string());
  REQUIRE(cpp_conn.has_value());
  REQUIRE(planar::db::apply_all(*cpp_conn));

  auto cpp_stmt = cpp_conn->prepare("select version, description from schema_migrations order by version");
  REQUIRE(cpp_stmt.has_value());

  std::size_t rows = 0;
  for (;;) {
    auto cpp_step = cpp_stmt->step();
    REQUIRE(cpp_step.has_value());
    if (*cpp_step == planar::db::step_result::done) {
      break;
    }
    REQUIRE(rows < k_expected.size());
    INFO("row " << rows);
    REQUIRE(cpp_stmt->column_int64(0) == k_expected[rows].first);
    REQUIRE(cpp_stmt->column_text(1) == k_expected[rows].second);
    ++rows;
  }
  REQUIRE(rows == 36);
}

TEST_CASE("migration 34 preserves legacy file annotations and guards entity annotation rollback",
          "[db][migrate][entity-annotations]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 36);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 33)));
  REQUIRE(conn->execute(
      "insert into annotations (scope_kind, anchor_path, anchor_text, title, body, status, vendor, created_at, updated_at) "
      "values ('global', 'legacy.cpp', 'legacy text', 'legacy title', 'legacy body', 'resolved', 'legacy', "
      "'2000-01-01T00:00:00.000Z', '2000-01-01T00:00:00.000Z');"));
  REQUIRE(conn->execute("insert into annotation_tags (annotation_id, tag) values (1, 'legacy-tag');"));

  REQUIRE(planar::db::apply_all(*conn));
  auto legacy = conn->prepare(
      "select anchor_kind, anchor_path, anchor_text, title, body, status, vendor, revision from annotations where id = 1");
  REQUIRE(legacy.has_value());
  REQUIRE(legacy->step().value() == planar::db::step_result::row);
  CHECK(legacy->column_text(0) == "file");
  CHECK(legacy->column_text(1) == "legacy.cpp");
  CHECK(legacy->column_text(2) == "legacy text");
  CHECK(legacy->column_text(3) == "legacy title");
  CHECK(legacy->column_text(4) == "legacy body");
  CHECK(legacy->column_text(5) == "resolved");
  CHECK(legacy->column_text(6) == "legacy");
  CHECK(legacy->column_int64(7) == 1);
  REQUIRE(legacy->step().value() == planar::db::step_result::done);

  auto legacy_tag = conn->prepare("select tag from annotation_tags where annotation_id = 1");
  REQUIRE(legacy_tag.has_value());
  REQUIRE(legacy_tag->step().value() == planar::db::step_result::row);
  CHECK(legacy_tag->column_text(0) == "legacy-tag");
  REQUIRE(legacy_tag->step().value() == planar::db::step_result::done);

  REQUIRE(conn->execute(chain[35].down_sql_));
  REQUIRE(conn->execute(chain[34].down_sql_));
  REQUIRE(conn->execute(chain[33].down_sql_));
  auto rolled_back_tag = conn->prepare("select tag from annotation_tags where annotation_id = 1");
  REQUIRE(rolled_back_tag.has_value());
  REQUIRE(rolled_back_tag->step().value() == planar::db::step_result::row);
  CHECK(rolled_back_tag->column_text(0) == "legacy-tag");
  REQUIRE(rolled_back_tag->step().value() == planar::db::step_result::done);

  REQUIRE(planar::db::apply_all(*conn));
  auto re_migrated_tag = conn->prepare("select tag from annotation_tags where annotation_id = 1");
  REQUIRE(re_migrated_tag.has_value());
  REQUIRE(re_migrated_tag->step().value() == planar::db::step_result::row);
  CHECK(re_migrated_tag->column_text(0) == "legacy-tag");
  REQUIRE(re_migrated_tag->step().value() == planar::db::step_result::done);

  auto identity = conn->prepare("select source_uuid from annotation_source_identity where singleton = 1");
  REQUIRE(identity.has_value());
  REQUIRE(identity->step().value() == planar::db::step_result::row);
  CHECK(identity->column_text(0).size() == 32);
  const auto source_uuid = identity->column_text(0);
  REQUIRE(identity->step().value() == planar::db::step_result::done);

  REQUIRE_FALSE(conn->execute("update annotation_source_identity set source_uuid = 'replaced' where singleton = 1").has_value());
  REQUIRE_FALSE(conn->execute("delete from annotation_source_identity where singleton = 1").has_value());
  auto preserved_identity = conn->prepare("select source_uuid from annotation_source_identity where singleton = 1");
  REQUIRE(preserved_identity.has_value());
  REQUIRE(preserved_identity->step().value() == planar::db::step_result::row);
  CHECK(preserved_identity->column_text(0) == source_uuid);
  REQUIRE(preserved_identity->step().value() == planar::db::step_result::done);

  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global', 'target', 'target', 'draft');"));
  REQUIRE(conn->execute("insert into annotations (scope_kind, anchor_kind, anchor_path, target_kind, target_id, body, plan_id) "
                        "values ('global', 'entity', null, 'plan', 1, 'durable note', 1);"));
  REQUIRE(conn->execute(chain[35].down_sql_));
  REQUIRE(conn->execute(chain[34].down_sql_));
  REQUIRE_FALSE(conn->execute(chain[33].down_sql_).has_value());
  auto guard = conn->prepare("select count(*) from sqlite_master where type = 'table' and name = 'annotation_rollback_guard'");
  REQUIRE(guard.has_value());
  REQUIRE(guard->step().value() == planar::db::step_result::row);
  CHECK(guard->column_int64(0) == 0);
  REQUIRE(guard->step().value() == planar::db::step_result::done);

  auto version = conn->prepare("select max(version) from schema_migrations");
  REQUIRE(version.has_value());
  REQUIRE(version->step().value() == planar::db::step_result::row);
  // Migrations 36 and 35 have rolled back, while migration 34 correctly refuses to
  // discard entity-anchored annotations. Its migration marker must remain.
  CHECK(version->column_int64(0) == 34);
  REQUIRE(version->step().value() == planar::db::step_result::done);

  REQUIRE(conn->execute("delete from annotations where anchor_kind = 'entity'"));
  REQUIRE(conn->execute(chain[33].down_sql_));
  REQUIRE(planar::db::apply_all(*conn));
}

TEST_CASE("migration 36 preserves prior invocation rows and refuses to discard busy diagnostics",
          "[db][migrate][cli-invocations][busy]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 36);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 35)));
  REQUIRE(
      conn->execute("insert into cli_invocations "
                    "(verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at) "
                    "values ('annotate command', '--request', 1, 'internal', 'project:demo', 7, '2000-01-01T00:00:00.000Z')"));

  REQUIRE(planar::db::apply_all(*conn));
  auto preserved = conn->prepare("select verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at "
                                 "from cli_invocations where id = 1");
  REQUIRE(preserved.has_value());
  REQUIRE(preserved->step().value() == planar::db::step_result::row);
  CHECK(preserved->column_text(0) == "annotate command");
  CHECK(preserved->column_text(1) == "--request");
  CHECK(preserved->column_int64(2) == 1);
  CHECK(preserved->column_text(3) == "internal");
  CHECK(preserved->column_text(4) == "project:demo");
  CHECK(preserved->column_int64(5) == 7);
  CHECK(preserved->column_text(6) == "2000-01-01T00:00:00.000Z");

  REQUIRE(conn->execute("insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at) "
                        "values ('annotate command', '--request', 1, 'busy', '2000-01-01T00:00:01.000Z')"));
  REQUIRE_FALSE(conn->execute(chain[35].down_sql_));

  auto marker = conn->prepare("select max(version) from schema_migrations");
  REQUIRE(marker.has_value());
  REQUIRE(marker->step().value() == planar::db::step_result::row);
  CHECK(marker->column_int64(0) == 36);
}

TEST_CASE("up-down-up roundtrip is lossless at every version in the chain", "[db][migrate][roundtrip]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 36);

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
  REQUIRE(stmt->column_int64(1) == 36);
}

TEST_CASE("the down chain from head deletes schema_migrations rows in strict descending mirror order, "
          "leaving no orphan rows, and a full rollback reaches an empty database",
          "[db][migrate][rollback]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 36);

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

TEST_CASE("the embedded migration chain is CONTIGUOUS, not merely monotonic", "[db][migrations][guard]") {
  // Monotonic is not contiguous (task 6058 acceptance criterion 2). A
  // chain of 1,2,5 is strictly increasing and would pass the monotonicity
  // case above, yet applying it produces a schema no version number
  // describes: `max(version)` reports 5 while migrations 3 and 4 never
  // ran, so every later handshake reads "version 5" and believes the
  // database has structure it does not have.
  REQUIRE(planar::db::require_contiguous(planar::db::migrations()).has_value());

  const std::array<planar::db::migration_record, 3> holed{
      planar::db::migration_record{.version_ = 1, .name_ = "a", .up_sql_ = "", .down_sql_ = ""},
      planar::db::migration_record{.version_ = 2, .name_ = "b", .up_sql_ = "", .down_sql_ = ""},
      planar::db::migration_record{.version_ = 5, .name_ = "c", .up_sql_ = "", .down_sql_ = ""},
  };
  auto const rejected = planar::db::require_contiguous(holed);
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().message_.contains("version 5"));

  // Not-starting-at-1 is the same defect at the other end of the chain.
  const std::array<planar::db::migration_record, 1> offset{
      planar::db::migration_record{.version_ = 7, .name_ = "a", .up_sql_ = "", .down_sql_ = ""},
  };
  REQUIRE_FALSE(planar::db::require_contiguous(offset).has_value());
}

TEST_CASE("apply_contiguous REFUSES a non-contiguous chain before running anything", "[db][migrate][guard][6697]") {
  // Task 6697. The contiguity PREDICATE was pinned -- a case calls
  // `require_contiguous` directly -- but its ENFORCEMENT POINT was not:
  // deleting the call from `apply_all(connection&)` rebuilt clean and passed
  // 3429 tests, exit 0. It could not fail, because the embedded chain IS
  // contiguous, so the guard never fires in production.
  //
  // `apply_contiguous` exists so the enforcement is reachable with a chain a
  // test controls. The span overload of `apply_all` deliberately does NOT
  // enforce (it is the seam for partial chains), so this is the only place
  // the wiring can be observed.
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  // A chain that SKIPS the first record: versions 2..4 with no 1.
  auto const gapped = planar::db::migrations().subspan(1, 3);
  auto const result = planar::db::apply_contiguous(*conn, gapped);
  REQUIRE_FALSE(result.has_value());

  // ASSERT WHICH FAILURE. `REQUIRE_FALSE` alone is not discriminating here,
  // and a first cut of this case was INERT for exactly that reason: with the
  // guard unwired, `apply_all` runs migration 2 without migration 1 and
  // fails on its own SQL, so the call returns an error either way. Only the
  // MESSAGE separates "refused by the guard" from "blew up downstream".
  CHECK(result.error().message_.contains("migration chain is not contiguous"));
  CHECK(result.error().message_.contains("position 0 holds version 2"));

  // And refused BEFORE running anything.
  auto probe = conn->prepare("select count(*) from sqlite_master where type='table' and name='schema_migrations'");
  REQUIRE(probe.has_value());
  REQUIRE(probe->step());
  CHECK(probe->column_int64(0) == 0);
}

TEST_CASE("apply_contiguous ACCEPTS the embedded chain, so the refusal above is not vacuous", "[db][migrate][guard][6697]") {
  // Non-vacuity for the case above: without this, an `apply_contiguous` that
  // refused every chain unconditionally would still look correct.
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  CHECK(planar::db::apply_contiguous(*conn, planar::db::migrations()).has_value());
}

TEST_CASE("assert_schema_compatible reports current / behind / ahead / gap", "[db][migrate][guard]") {
  auto const head = planar::db::embedded_max();
  REQUIRE(head == 36);

  SECTION("a fresh, never-initialized database is BEHIND at version 0") {
    scratch_db_path scratch;
    auto            conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());

    auto const state = planar::db::assert_schema_compatible(*conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == 0);
    CHECK(state->embedded_max_ == head);
    CHECK(state->verdict_ == planar::db::schema_compatibility::behind);
  }

  SECTION("a fully migrated database is CURRENT") {
    scratch_db_path scratch;
    auto            conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn));

    auto const state = planar::db::assert_schema_compatible(*conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == head);
    CHECK(state->verdict_ == planar::db::schema_compatibility::current);
  }

  SECTION("a database migrated by a newer binary is AHEAD") {
    scratch_db_path scratch;
    auto            conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn));
    REQUIRE(conn->execute(std::format("insert into schema_migrations (version, description) values ({}, 'newer');", head + 1)));

    auto const state = planar::db::assert_schema_compatible(*conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == head + 1);
    CHECK(state->verdict_ == planar::db::schema_compatibility::ahead);
  }

  SECTION("a hole in the applied set is a GAP, and outranks BEHIND") {
    // The failure this arm exists for: `apply_all` decides what to run
    // from `max(version)` alone, so a database missing migration 5 but
    // carrying rows up to 20 looks exactly like a healthy version-20
    // database. Nothing re-runs 5, and the binary proceeds against a
    // schema that is missing whatever 5 created.
    scratch_db_path scratch;
    auto            conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn, planar::db::migrations().subspan(0, 20)));
    REQUIRE(conn->execute("delete from schema_migrations where version = 5;"));

    auto const state = planar::db::assert_schema_compatible(*conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == 20);
    CHECK(state->verdict_ == planar::db::schema_compatibility::gap);
  }

  SECTION("a NON-POSITIVE version is a gap that the count/max comparison alone cannot see") {
    // Task 6696. The `lowest != 1` half of `has_hole` had NO fixture: a
    // reviewer probe deleting it left `ctest -L db` at 31/31. It is very
    // nearly redundant -- with unique versions >= 1, `applied == live`
    // already implies the set is exactly 1..live, so `lowest` must be 1 --
    // and `schema_migrations.version` carries no CHECK constraint, so the
    // ONLY way to reach the clause is a row at version <= 0.
    //
    // Constructed so the OTHER half is false: versions {0, 2, 3} give
    // count == 3 and max == 3, so `applied != live_` does NOT fire. Only
    // `lowest != 1` catches it. That makes this the exact fixture the
    // surviving probe was missing.
    scratch_db_path scratch;
    auto            conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn, planar::db::migrations().subspan(0, 3)));
    REQUIRE(conn->execute("delete from schema_migrations where version = 1;"));
    REQUIRE(conn->execute("insert into schema_migrations (version, description) values (0, 'hand-corrupted');"));

    // Non-vacuity: the arithmetic the clause depends on really does hide
    // this from the count/max half.
    auto checks = conn->prepare("select max(version), count(*), min(version) from schema_migrations");
    REQUIRE(checks.has_value());
    REQUIRE(checks->step());
    CHECK(checks->column_int64(0) == 3); // live_
    CHECK(checks->column_int64(1) == 3); // applied == live_, so that half is FALSE
    CHECK(checks->column_int64(2) == 0); // lowest != 1, so this half is the only one left

    auto const state = planar::db::assert_schema_compatible(*conn);
    REQUIRE(state.has_value());
    CHECK(state->live_ == 3);
    CHECK(state->verdict_ == planar::db::schema_compatibility::gap);
  }

  SECTION("a gap ALSO outranks current, and AHEAD outranks a gap") {
    scratch_db_path scratch;
    auto            conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn));
    REQUIRE(conn->execute("delete from schema_migrations where version = 5;"));

    auto const gapped = planar::db::assert_schema_compatible(*conn);
    REQUIRE(gapped.has_value());
    CHECK(gapped->verdict_ == planar::db::schema_compatibility::gap);

    REQUIRE(conn->execute(std::format("insert into schema_migrations (version, description) values ({}, 'newer');", head + 1)));
    auto const ahead = planar::db::assert_schema_compatible(*conn);
    REQUIRE(ahead.has_value());
    CHECK(ahead->verdict_ == planar::db::schema_compatibility::ahead);
  }
}
