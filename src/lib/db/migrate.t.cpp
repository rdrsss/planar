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
  REQUIRE(stmt->column_int64(1) == 41);
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
  auto const chain = planar::db::migrations();
  REQUIRE_FALSE(chain.empty());
  for (auto const& record : chain) {
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
  // `apply_all` and must fail right at `begin_transaction` (a genuine
  // post-timeout SQLITE_BUSY) rather than partway through executing a
  // migration script.
  constexpr int k_sqlite_busy = 5; // SQLITE_BUSY

  scratch_db_path scratch;

  auto conn_a = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_a.has_value());
  auto conn_b = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_b.has_value());
  // Task 6842: connection::open now sets busy_timeout=5000 by default.
  // Lower B's override so the guaranteed-busy assertion below (A never
  // releases its lock during the scope) doesn't block for 5 real seconds.
  REQUIRE(conn_b->execute("pragma busy_timeout = 50;"));

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
  REQUIRE(stmt->column_int64(1) == 41);
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
      {37, "drop the four dormant slug columns added by 00011"},
      {38, "execution supervision: claim supervisor/attempt, run engine, nullable run plan, supervision action kinds"},
      {39, "workflow_runs: nullable pid, lease expires_at, CHECK one of pid/expires_at is set (decision D11)"},
      {40, "host queue: queue_entries, queue_history, queue_schema marker, sequence floor 1000000"},
      {41, "contextual annotation threads, revisioned messages, and adjacent range anchors"},
  };
  REQUIRE(k_expected.size() == 41);

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
  REQUIRE(rows == 41);
}

TEST_CASE("migration 34 preserves legacy file annotations and guards entity annotation rollback",
          "[db][migrate][entity-annotations]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
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

  // Peel from head so the chain stays contiguous for the re-apply below.
  REQUIRE(conn->execute(chain[40].down_sql_));
  REQUIRE(conn->execute(chain[39].down_sql_));
  REQUIRE(conn->execute(chain[38].down_sql_));
  REQUIRE(conn->execute(chain[37].down_sql_));
  REQUIRE(conn->execute(chain[36].down_sql_));
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

  // Exercise migration 34's entity rollback guard in its own schema. A note
  // created at schema 41 has an independent message identity and correctly
  // cannot pass migration 41's lossless downgrade guard.
  REQUIRE(conn->execute(chain[40].down_sql_));
  REQUIRE(conn->execute(chain[39].down_sql_));
  REQUIRE(conn->execute(chain[38].down_sql_));
  REQUIRE(conn->execute(chain[37].down_sql_));
  REQUIRE(conn->execute(chain[36].down_sql_));
  REQUIRE(conn->execute(chain[35].down_sql_));
  REQUIRE(conn->execute(chain[34].down_sql_));
  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global', 'target', 'target', 'draft');"));
  REQUIRE(conn->execute("insert into annotations (scope_kind, anchor_kind, anchor_path, target_kind, target_id, body, plan_id) "
                        "values ('global', 'entity', null, 'plan', 1, 'durable note', 1);"));
  REQUIRE_FALSE(conn->execute(chain[33].down_sql_).has_value());
  auto guard = conn->prepare("select count(*) from sqlite_master where type = 'table' and name = 'annotation_rollback_guard'");
  REQUIRE(guard.has_value());
  REQUIRE(guard->step().value() == planar::db::step_result::row);
  CHECK(guard->column_int64(0) == 0);
  REQUIRE(guard->step().value() == planar::db::step_result::done);

  auto version = conn->prepare("select max(version) from schema_migrations");
  REQUIRE(version.has_value());
  REQUIRE(version->step().value() == planar::db::step_result::row);
  // Migrations 40, 39, 38, 37, 36 and 35 have rolled back, while migration 34 correctly refuses to
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
  REQUIRE(chain.size() == 41);
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
  CHECK(marker->column_int64(0) == 41);
}

namespace {

/// @brief Run a single-value query and return it as text. Callers fold any
/// NULL into text in the SQL itself (`ifnull`), so the value is never NULL.
/// @param conn The connection.
/// @param sql A query yielding exactly one row and one column.
/// @return The value.
auto scalar(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  INFO(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  auto value = stmt->column_text(0);
  REQUIRE(stmt->step().value() == planar::db::step_result::done);
  return value;
}

} // namespace

TEST_CASE("migration 38 adds engine supervision without disturbing any row that references the rebuilt-in-place tables",
          "[db][migrate][6487]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
  REQUIRE(chain[37].version_ == 38);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 37)));

  // Rows in every table that REFERENCES workflow_runs or agent_actions. The
  // obvious table-rebuild for this migration was measured to CASCADE-delete
  // the context_records rows and NULL the claims' run_id (see the up file);
  // these rows are what would have caught it.
  for (auto const* sql : {
           "insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'p', 'active')",
           "insert into sessions (vendor, started_at) values ('claude', '2000-01-01T00:00:00.000Z')",
           "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) "
           "values (1, 'w', 'r1', 1, '/r'), (1, 'w', 'r2', 2, '/r')",
           "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, vendor, lease_expires_at, run_id) "
           "values ('t1', 1, 'task', 1, 'claude', 'x', 1), ('t2', 1, 'task', 2, 'claude', 'x', 2)",
           "insert into context_records (run_id, stage, session_id, claim_id, kind, body) "
           "values (1, 's', 1, 1, 'finding', 'b'), (2, 's', 1, 2, 'finding', 'b')",
           "insert into agent_actions (session_id, claim_id, action_kind, vendor) values (1, 1, 'coder', 'claude')",
           "insert into agent_actions (session_id, parent_action_id, claim_id, action_kind, vendor) "
           "values (1, 1, 2, 'reviewer', 'claude')",
       }) {
    INFO(sql);
    REQUIRE(conn->execute(sql));
  }

  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 38)));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "38");

  // Nothing referencing the two edited tables moved.
  CHECK(scalar(*conn, "select count(*) from context_records") == "2");
  CHECK(scalar(*conn, "select group_concat(run_id) from (select run_id from agent_work_claims order by id)") == "1,2");
  CHECK(scalar(*conn, "select group_concat(ifnull(parent_action_id, '-')) from (select * from agent_actions order by id)") ==
        "-,1");
  CHECK(scalar(*conn, "select count(*) from pragma_foreign_key_check") == "0");
  CHECK(scalar(*conn, "pragma integrity_check") == "ok");
  // ...and every child still REFERENCES the live table by its own name.
  CHECK(scalar(*conn, "select \"table\" from pragma_foreign_key_list('agent_work_claims') where \"from\" = 'run_id'") ==
        "workflow_runs");
  CHECK(scalar(*conn, "select \"table\" from pragma_foreign_key_list('context_records') where \"from\" = 'run_id'") ==
        "workflow_runs");
  CHECK(scalar(*conn, "select \"table\" from pragma_foreign_key_list('agent_actions') where \"from\" = 'parent_action_id'") ==
        "agent_actions");

  // Pre-existing rows read as caller-supervised, embedded, un-attempted.
  CHECK(scalar(*conn, "select group_concat(supervisor || ':' || ifnull(attempt_id, 'null')) from agent_work_claims") ==
        "caller:null,caller:null");
  CHECK(scalar(*conn, "select group_concat(engine) from workflow_runs") == "embedded,embedded");

  // The relaxed constraints hold on THIS connection, straight after the
  // migration ran on it (the up file makes the connection re-read its schema).
  REQUIRE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, engine) "
                        "values (null, 'w', 'r-null', 3, '/r', 'centurion')"));
  for (auto const* kind : {"claim_associate", "claim_terminal", "supervisor_override", "run_submitted", "run_reconciled"}) {
    INFO(kind);
    REQUIRE(
        conn->execute(std::format("insert into agent_actions (session_id, action_kind, vendor) values (1, '{}', 'c')", kind)));
  }
  REQUIRE(conn->execute("update agent_work_claims set supervisor = 'engine', attempt_id = 'A1' where id = 1"));

  // ...and the new CHECKs refuse what they should.
  CHECK_FALSE(conn->execute("update agent_work_claims set supervisor = 'both' where id = 1").has_value());
  CHECK_FALSE(conn->execute("update workflow_runs set engine = 'zig' where id = 1").has_value());
  CHECK_FALSE(conn->execute("insert into agent_actions (session_id, action_kind, vendor) values (1, 'bogus', 'c')").has_value());

  // Rollback refuses BY NAME while a plan-less run exists, and changes nothing.
  auto refused = conn->execute(chain[37].down_sql_);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().message_.contains("m00038_down_refused_null_plan_run_or_supervision_action_kind_present"));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "38");
  CHECK(scalar(*conn, "select count(*) from pragma_table_info('agent_work_claims') where name in ('supervisor', 'attempt_id')") ==
        "2");

  // Still refused while only a new-kind action remains.
  REQUIRE(conn->execute("delete from workflow_runs where plan_id is null"));
  REQUIRE_FALSE(conn->execute(chain[37].down_sql_).has_value());
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "38");

  // Disposed of: rollback succeeds, the constraints are back ON THIS
  // connection, and the referencing rows are still intact.
  REQUIRE(conn->execute("delete from agent_actions where action_kind in ('claim_associate', 'claim_terminal', "
                        "'supervisor_override', 'run_submitted', 'run_reconciled')"));
  REQUIRE(conn->execute(chain[37].down_sql_));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "37");
  CHECK_FALSE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) "
                            "values (null, 'w', 'r-null2', 4, '/r')")
                  .has_value());
  CHECK_FALSE(
      conn->execute("insert into agent_actions (session_id, action_kind, vendor) values (1, 'claim_terminal', 'c')").has_value());
  CHECK(scalar(*conn, "select count(*) from context_records") == "2");
  CHECK(scalar(*conn, "select group_concat(run_id) from (select run_id from agent_work_claims order by id)") == "1,2");
  CHECK(scalar(*conn, "select count(*) from pragma_foreign_key_check") == "0");

  // And forward again — through migrations 39, 40 and 41 too, confirming the
  // tail of the chain still applies cleanly on top of 38's re-applied shape.
  REQUIRE(planar::db::apply_all(*conn));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "41");
}

TEST_CASE("migration 39 makes workflow_runs.pid nullable behind a lease CHECK, without a table rebuild", "[db][migrate][6846]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
  REQUIRE(chain[38].version_ == 39);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 38)));

  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'p', 'active')"));
  // A pid-bound run, seeded BEFORE migration 39 runs.
  REQUIRE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) "
                        "values (1, 'w', 'r1', 4242, '/r')"));

  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 39)));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "39");

  // The pre-existing row survives untouched: still pid 4242, still `running`,
  // no expires_at, and the CHECK (which reads it as pid-supervised) admits it.
  CHECK(scalar(*conn, "select pid from workflow_runs where id = 1") == "4242");
  CHECK(scalar(*conn, "select status from workflow_runs where id = 1") == "running");
  CHECK(scalar(*conn, "select ifnull(expires_at, 'null') from workflow_runs where id = 1") == "null");

  // A pid-less, lease-supervised run is now representable.
  REQUIRE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, expires_at, repo_root) "
                        "values (1, 'w', 'r2', '2099-01-01T00:00:00.000Z', '/r')"));
  CHECK(scalar(*conn, "select count(*) from workflow_runs") == "2");

  // ...but a row with NEITHER pid NOR expires_at violates the new CHECK.
  CHECK_FALSE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, repo_root) "
                            "values (1, 'w', 'r3', '/r')")
                  .has_value());
  CHECK(scalar(*conn, "select count(*) from workflow_runs") == "2");

  // The new partial index exists and is scoped to running rows.
  CHECK(scalar(*conn, "select count(*) from sqlite_master where type = 'index' and name = 'ix_workflow_runs_expires'") == "1");

  // Nothing referencing workflow_runs moved (no table rebuild happened).
  CHECK(scalar(*conn, "select count(*) from pragma_foreign_key_check") == "0");
  CHECK(scalar(*conn, "pragma integrity_check") == "ok");

  // Down: the NULL-pid row is deleted, then pid's NOT NULL is restored.
  REQUIRE(conn->execute(chain[38].down_sql_));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "38");
  CHECK(scalar(*conn, "select count(*) from workflow_runs") == "1");
  CHECK(scalar(*conn, "select pid from workflow_runs where id = 1") == "4242");
  CHECK(scalar(*conn, "select count(*) from pragma_table_info('workflow_runs') where name = 'expires_at'") == "0");
  CHECK_FALSE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, repo_root) "
                            "values (1, 'w', 'r4', '/r')")
                  .has_value());

  // And forward again.
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 39)));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "39");
}

TEST_CASE("migration 38's up guard FIRES when the stored workflow_runs CREATE TABLE text does not match "
          "the expected literal, refusing by name and leaving schema_migrations at 37",
          "[db][migrate][6905]") {
  // Neither 00038's nor 00039's TEMP-table guard had a test proving it
  // actually FIRES on a mismatch -- only that it stays silent on the
  // ordinary path (the "adds engine supervision..." case above). This
  // pins the negative: perturb the stored DDL text migration 38's
  // `replace()` targets so it cannot match, and require the migration to
  // fail by its own named constraint rather than silently no-op the
  // relaxation and still record version 38.
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
  REQUIRE(chain[37].version_ == 38);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 37)));

  // Perturb the stored `workflow_runs` CREATE TABLE text so migration 38's
  // first `replace()` (targeting the literal
  // `plan_id       integer not null references plans(id)`) cannot match --
  // as if an earlier, unrelated edit had reformatted the column
  // declaration's whitespace. `replace()` on a non-match is a silent
  // no-op in SQLite, so absent the guard this would record version 38 over
  // an UNRELAXED `plan_id` constraint.
  REQUIRE(conn->execute("pragma writable_schema = on;"));
  REQUIRE(conn->execute("update sqlite_schema set sql = replace(sql, "
                        "'plan_id       integer not null references plans(id)', "
                        "'plan_id integer not null references plans(id)') "
                        "where type = 'table' and name = 'workflow_runs';"));
  REQUIRE(conn->execute("pragma writable_schema = reset;"));
  CHECK(scalar(*conn, "select sql like '%plan_id       integer not null references plans(id)%' "
                      "from sqlite_schema where type = 'table' and name = 'workflow_runs'") == "0");

  auto const applied38 = planar::db::apply_all(*conn, chain.subspan(0, 38));
  REQUIRE_FALSE(applied38.has_value());
  CHECK(
      applied38.error().message_.contains("m00038_up_refused_unexpected_stored_schema_text_plan_id_or_action_kind_not_relaxed"));

  // Refused BY NAME, and the version row was never written -- the failing
  // migration's own transaction rolled back.
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "37");
}

TEST_CASE("migration 39's up guard FIRES when the stored workflow_runs CREATE TABLE text does not match "
          "the expected literal, refusing by name and leaving schema_migrations at 38",
          "[db][migrate][6905]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
  REQUIRE(chain[38].version_ == 39);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 38)));

  // Perturb the stored `workflow_runs` CREATE TABLE text so migration 39's
  // first `replace()` (targeting the literal `pid           integer not
  // null,`) cannot match.
  REQUIRE(conn->execute("pragma writable_schema = on;"));
  REQUIRE(conn->execute("update sqlite_schema set sql = replace(sql, "
                        "'pid           integer not null,', "
                        "'pid integer not null,') "
                        "where type = 'table' and name = 'workflow_runs';"));
  REQUIRE(conn->execute("pragma writable_schema = reset;"));
  CHECK(scalar(*conn, "select sql like '%pid           integer not null,%' "
                      "from sqlite_schema where type = 'table' and name = 'workflow_runs'") == "0");

  auto const applied39 = planar::db::apply_all(*conn, chain.subspan(0, 39));
  REQUIRE_FALSE(applied39.has_value());
  CHECK(applied39.error().message_.contains("m00039_up_refused_pid_not_relaxed_or_lease_check_missing"));

  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "38");
}

TEST_CASE("up-down-up roundtrip is lossless at every version in the chain", "[db][migrate][roundtrip]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);

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
  REQUIRE(stmt->column_int64(1) == 41);
}

TEST_CASE("migration 00037 drops exactly the four dormant slug columns and their indexes", "[db][migrate][6808]") {
  // Migration 00011 gave five tables a `slug`; only `tasks.slug` was ever
  // read or written. 00037 removes the other four (task 6808). Pin both
  // directions: head has no such column, and rolling 00037 back alone
  // restores each column AND its partial unique index, so a downgraded
  // binary that expects the 00011 shape still finds it.
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));

  auto has_column = [&](std::string_view table) {
    auto stmt = conn->prepare(std::format("select count(*) from pragma_table_info('{}') where name = 'slug'", table));
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().value() == planar::db::step_result::row);
    return stmt->column_int64(0) == 1;
  };
  auto has_index = [&](std::string_view name) {
    auto stmt = conn->prepare(std::format("select count(*) from sqlite_master where type = 'index' and name = '{}'", name));
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().value() == planar::db::step_result::row);
    return stmt->column_int64(0) == 1;
  };

  for (auto table : {"artifacts", "questions", "test_scenarios", "decisions"}) {
    INFO(table);
    CHECK_FALSE(has_column(table));
    CHECK_FALSE(has_index(std::format("ux_{}_slug", table)));
  }
  // The live slugs are untouched.
  CHECK(has_column("tasks"));
  CHECK(has_index("ux_tasks_slug"));
  CHECK(has_column("plans"));
  CHECK(has_column("annotations"));
  CHECK(has_index("ux_annotations_slug"));

  const auto chain = planar::db::migrations();
  // Peel 00041, 00040, 00039 and 00038 first so 00037's down runs against the schema it was written for.
  REQUIRE(chain[36].version_ == 37);
  REQUIRE(conn->execute(chain[40].down_sql_));
  REQUIRE(conn->execute(chain[39].down_sql_));
  REQUIRE(conn->execute(chain[38].down_sql_));
  REQUIRE(conn->execute(chain[37].down_sql_));
  REQUIRE(conn->execute(chain[36].down_sql_));
  for (auto table : {"artifacts", "questions", "test_scenarios", "decisions"}) {
    INFO(table);
    CHECK(has_column(table));
    CHECK(has_index(std::format("ux_{}_slug", table)));
  }
}

TEST_CASE("migration 00041 losslessly promotes entity notes to page threads and preserves file notes",
          "[db][migrate][6765][annotation-thread]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
  REQUIRE(chain[40].version_ == 41);
  REQUIRE(planar::db::apply_all(*conn, chain.subspan(0, 40)));

  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug, status) values (91, 'global', 'thread target', "
                        "'thread-target', 'draft')"));
  REQUIRE(
      conn->execute("insert into annotations (id, scope_kind, anchor_kind, target_kind, target_id, body, status, vendor, origin, "
                    "revision, plan_id, created_at, updated_at) values "
                    "(701, 'global', 'entity', 'plan', 91, 'legacy entity', 'resolved', 'codex', 'explorer', 7, 91, "
                    "'2000-01-01T00:00:00.000Z', '2000-01-02T00:00:00.000Z')"));
  REQUIRE(conn->execute("insert into annotations (id, scope_kind, anchor_kind, anchor_path, body, revision) "
                        "values (990, 'global', 'file', 'src/a.cpp', 'legacy file', 3)"));
  REQUIRE(conn->execute("insert into annotation_tags (annotation_id, tag) values (701, 'review')"));

  REQUIRE(planar::db::apply_all(*conn));
  CHECK(scalar(*conn, "select count(*) from annotation_messages") == "1");
  CHECK(scalar(*conn, "select id || ':' || annotation_id || ':' || revision || ':' || body from annotation_messages") ==
        "701:701:7:legacy entity");
  CHECK(scalar(*conn, "select count(*) from annotation_messages where annotation_id = 990") == "0");
  CHECK(scalar(*conn, "select tag from annotation_tags where annotation_id = 701") == "review");
  CHECK(scalar(*conn, "select count(*) from annotation_contextual_anchors") == "0");
  CHECK(scalar(*conn, "select message_id || ':' || revision || ':' || body from annotation_message_revisions") ==
        "701:7:legacy entity");
  REQUIRE_FALSE(conn->execute("update annotation_message_revisions set body = 'lost' where message_id = 701"));

  // The untouched migrated shape is exactly reversible.
  REQUIRE(conn->execute(chain[40].down_sql_));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "40");
  REQUIRE(planar::db::apply_all(*conn));

  auto anchor_insert =
      conn->execute("insert into annotation_contextual_anchors (annotation_id, schema_version, document_kind, document_id, "
                    "document_version, content_revision, anchor_kind, start_block_key, end_block_key, start_offset, end_offset, "
                    "normalized_quote) "
                    "values (701, 1, 'plan', 91, 1, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 'range', "
                    "'p:1', 'p:2', 2, 6, 'alpha beta')");
  const auto anchor_insert_detail = anchor_insert.has_value() ? std::string{"ok"} : anchor_insert.error().message_;
  INFO(anchor_insert_detail);
  REQUIRE(anchor_insert.has_value());
  REQUIRE(conn->execute("insert into annotation_anchor_segments (annotation_id, ordinal, block_key, quote) values "
                        "(701, 0, 'p:1', 'alpha'), (701, 1, 'p:2', 'beta')"));
  REQUIRE_FALSE(conn->execute("insert into annotation_anchor_segments (annotation_id, ordinal, block_key, quote) "
                              "values (701, 3, 'p:4', 'discontinuous')"));
  REQUIRE_FALSE(conn->execute("update annotation_contextual_anchors set anchor_state = 'orphaned' where annotation_id = 701"));
  REQUIRE(conn->execute("update annotation_contextual_anchors set anchor_state = 'orphaned', revision = 2, "
                        "updated_at = '2002-01-01T00:00:00.000Z' where annotation_id = 701 and revision = 1"));
  CHECK(scalar(*conn, "select summary from audit_log where entity_kind = 'annotation' and entity_id = 701 "
                      "order by id desc limit 1") == "explicit contextual re-anchor");
  REQUIRE(conn->execute("delete from annotation_contextual_anchors where annotation_id = 701"));

  // Valid replies with identical timestamps are retained and replay in the
  // required (created_at,id) order. Once history exists, rollback refuses.
  REQUIRE(conn->execute("insert into annotation_messages (annotation_id, body, created_at, updated_at) values "
                        "(701, 'reply b', '2001-01-01T00:00:00.000Z', '2001-01-01T00:00:00.000Z'), "
                        "(701, 'reply c', '2001-01-01T00:00:00.000Z', '2001-01-01T00:00:00.000Z')"));
  CHECK(scalar(*conn, "select group_concat(body, '|') from (select body from annotation_messages "
                      "where annotation_id = 701 order by created_at, id)") == "legacy entity|reply b|reply c");
  REQUIRE_FALSE(conn->execute(chain[40].down_sql_));
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "41");
}

TEST_CASE("the down chain from head deletes schema_migrations rows in strict descending mirror order, "
          "leaving no orphan rows, and a full rollback reaches an empty database",
          "[db][migrate][rollback]") {
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));

  const auto chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);

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
  REQUIRE(head == 41);

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

namespace {

/// @brief The `(version, name)` pairs of every `*.up.sql` under `dir`,
/// read from the source tree at test time, so a chain can be compared with
/// the DIRECTORY it claims to embed rather than with itself.
/// @param dir A migration stream directory (a `PLANAR_*_DIR` definition).
/// @return The sorted set of pairs; empty if the directory is missing.
auto up_files_in(std::filesystem::path const& dir) -> std::set<std::pair<std::uint32_t, std::string>> {
  std::set<std::pair<std::uint32_t, std::string>> out;
  std::error_code                                 ec;
  for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
    auto const name = entry.path().filename().string();
    if (!name.ends_with(".up.sql")) {
      continue;
    }
    auto const underscore = name.find('_');
    REQUIRE(underscore != std::string::npos);
    auto const version = static_cast<std::uint32_t>(std::stoul(name.substr(0, underscore)));
    out.emplace(version, name.substr(underscore + 1, name.size() - underscore - 1 - std::string_view(".up.sql").size()));
  }
  return out;
}

/// @brief The `(version, name)` pairs of an embedded chain, in the same
/// shape as `up_files_in` so the two can be compared directly.
/// @param chain An embedded migration chain.
/// @return The sorted set of pairs.
auto records_of(std::span<planar::db::migration_record const> chain) -> std::set<std::pair<std::uint32_t, std::string>> {
  std::set<std::pair<std::uint32_t, std::string>> out;
  for (auto const& record : chain) {
    out.emplace(record.version_, std::string(record.name_));
  }
  return out;
}

/// @brief `sql` with every `--` line comment removed, so a check over
/// what a migration DOES is not tripped by what its comments SAY.
/// @param sql A migration script.
/// @return The script's non-comment text.
auto sql_without_comments(std::string_view sql) -> std::string {
  std::string out;
  for (auto const line : std::views::split(sql, '\n')) {
    std::string_view const text(line.begin(), line.end());
    out += text.substr(0, text.find("--"));
    out += '\n';
  }
  return out;
}

/// @brief Whether a table exists on `conn`.
/// @param conn The connection.
/// @param table The table name.
/// @return `true` when `sqlite_master` lists it.
auto table_exists(planar::db::connection& conn, std::string_view table) -> bool {
  auto stmt = conn.prepare(std::format("select count(*) from sqlite_master where type = 'table' and name = '{}'", table));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  return stmt->column_int64(0) == 1;
}

} // namespace

TEST_CASE("the embedded main chain is exactly the files under migrations/ and writes only schema_migrations",
          "[db][migrations]") {
  // The main chain is compared with the DIRECTORY it claims to embed, so a
  // dropped or stale generated unit fails here rather than matching itself.
  // The retired agent stream is gone (plan 1089): no migration may name
  // `agent_schema_migrations`.
  auto const main = planar::db::migrations();
  REQUIRE_FALSE(main.empty());

  auto const main_files = up_files_in(PLANAR_MIGRATIONS_DIR);
  REQUIRE_FALSE(main_files.empty());
  CHECK(records_of(main) == main_files);
  CHECK(planar::db::require_contiguous(main).has_value());
  CHECK(planar::db::embedded_max(main) == main.size());

  for (auto const& record : main) {
    INFO("main migration " << record.version_ << " " << record.name_);
    CHECK(sql_without_comments(record.up_sql_).find("agent_schema_migrations") == std::string::npos);
    CHECK(sql_without_comments(record.down_sql_).find("agent_schema_migrations") == std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Plan 1089, task qp-migration: migration 00040 folds the host queue's two
// tables into planar.db, with the queue_schema marker and the sequence floor
// (tech spec 656 § Schema Changes, decisions 1219, 1220 and 1222).
// ---------------------------------------------------------------------------

namespace {

/// @brief One `name:type:notnull:default:pk` row per column of `table`, in
/// column order, so two tables compare equal only when every column, its
/// position and its declared shape agree.
/// @param conn The connection.
/// @param table The table to describe.
/// @return The rows joined with `,`; empty when the table does not exist.
auto table_shape(planar::db::connection& conn, std::string_view table) -> std::string {
  auto stmt = conn.prepare(std::format("select ifnull(group_concat(name || ':' || type || ':' || \"notnull\" || ':' || "
                                       "ifnull(dflt_value, 'none') || ':' || pk, ','), '') "
                                       "from (select * from pragma_table_info('{}') order by cid)",
                                       table));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// @brief Inserts one minimal `queue_entries` row and returns its `seq`.
/// @param conn The connection.
/// @return The sequence number the store assigned.
auto insert_queue_entry(planar::db::connection& conn) -> std::int64_t {
  REQUIRE(conn.execute("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, enqueued_at, refreshed_mono) "
                       "values ('waiting', 'h', 1, 1, '/', '[]', 0, 0)"));
  return std::stoll(scalar(conn, "select max(seq) from queue_entries"));
}

/// @brief The text of a markdown section: from the line `heading` to the
/// next heading of the same or a higher level, or to the end of the file.
/// @param doc The whole markdown text.
/// @param heading The heading line, `#` prefix included.
/// @return The section, heading included; empty when the heading is absent.
auto markdown_section(std::string_view doc, std::string_view heading) -> std::string {
  auto const at = doc.find(std::format("\n{}\n", heading));
  if (at == std::string_view::npos) {
    return {};
  }
  auto const level = heading.find_first_not_of('#');
  auto const body  = doc.substr(at + 1);
  auto       end   = std::string_view::npos;
  for (std::size_t pos = body.find("\n#"); pos != std::string_view::npos; pos = body.find("\n#", pos + 1)) {
    auto const hashes = body.substr(pos + 1).find_first_not_of('#');
    if (hashes <= level) {
      end = pos;
      break;
    }
  }
  return std::string(body.substr(0, end));
}

/// @brief Reads a whole file from the source tree.
/// @param path The file.
/// @return Its bytes; the case fails when it cannot be read.
auto read_source_file(std::filesystem::path const& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  INFO(path.string());
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("migration 40 creates queue_entries and queue_history in the agent v3 shape, the queue_schema marker and the "
          "sequence floor",
          "[db][migrate][queue][qp-migration]") {
  // Test-spec scenario "Happy path -- the queue migration creates the tables,
  // marker and sequence floor" (plan 1089, task qp-migration).
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));

  // Every column of the retired agent store's v3 shape (its migrations 00002
  // and 00003), column for column and in order. The reference stream is gone
  // with agent.db, so the shape is pinned here as literals: a column added,
  // dropped, retyped or reordered fails by name.
  CHECK(table_shape(*conn, "queue_entries") ==
        "seq:INTEGER:0:none:1,state:TEXT:1:none:0,host_id:TEXT:1:none:0,pid:INTEGER:1:none:0,pid_started:INTEGER:1:none:0,"
        "child_pgid:INTEGER:0:none:0,child_started:INTEGER:0:none:0,parent_seq:INTEGER:0:none:0,"
        "terminating_since_mono:INTEGER:0:none:0,terminate_reason:TEXT:0:none:0,cancelled_by:TEXT:0:none:0,cwd:TEXT:1:none:0,"
        "argv:TEXT:1:none:0,label:TEXT:0:none:0,vendor:TEXT:0:none:0,role:TEXT:0:none:0,claim_token:TEXT:0:none:0,"
        "log_path:TEXT:0:none:0,enqueued_at:INTEGER:1:none:0,started_at:INTEGER:0:none:0,refreshed_mono:INTEGER:1:none:0,"
        "deadline_mono:INTEGER:0:none:0,wait_deadline_mono:INTEGER:0:none:0,run_limit_ms:INTEGER:0:none:0,"
        "wait_limit_ms:INTEGER:0:none:0");
  CHECK(table_shape(*conn, "queue_history") ==
        "seq:INTEGER:0:none:1,outcome:TEXT:1:none:0,exit_code:INTEGER:0:none:0,signal:INTEGER:0:none:0,"
        "successor_seq:INTEGER:0:none:0,cancelled_by:TEXT:0:none:0,nested:INTEGER:1:0:0,parent_seq:INTEGER:0:none:0,"
        "cwd:TEXT:1:none:0,argv:TEXT:1:none:0,label:TEXT:0:none:0,vendor:TEXT:0:none:0,role:TEXT:0:none:0,"
        "log_path:TEXT:0:none:0,enqueued_at:INTEGER:1:none:0,started_at:INTEGER:0:none:0,ended_at:INTEGER:1:none:0,"
        "waited_ms:INTEGER:1:none:0,ran_ms:INTEGER:0:none:0,run_limit_ms:INTEGER:0:none:0,wait_limit_ms:INTEGER:0:none:0");
  CHECK(table_shape(*conn, "queue_entries").starts_with("seq:INTEGER:0:none:1,state:TEXT:1:none:0,"));
  CHECK(table_shape(*conn, "queue_entries").ends_with(",run_limit_ms:INTEGER:0:none:0,wait_limit_ms:INTEGER:0:none:0"));
  CHECK(table_shape(*conn, "queue_history").ends_with(",run_limit_ms:INTEGER:0:none:0,wait_limit_ms:INTEGER:0:none:0"));
  CHECK(table_shape(*conn, "queue_schema") == "version:INTEGER:0:none:1,compat:INTEGER:1:none:0,description:TEXT:1:none:0");

  // The three indexes, each on the column its name says.
  CHECK(scalar(*conn, "select group_concat(name || '=' || (select group_concat(name) from pragma_index_info(l.name)), ',') "
                      "from (select * from pragma_index_list('queue_entries') where origin = 'c' order by name) l") ==
        "idx_queue_entries_parent_seq=parent_seq,idx_queue_entries_state=state");
  CHECK(scalar(*conn, "select group_concat(name || '=' || (select group_concat(name) from pragma_index_info(l.name)), ',') "
                      "from (select * from pragma_index_list('queue_history') where origin = 'c' order by name) l") ==
        "idx_queue_history_ended_at=ended_at");

  // No foreign keys, triggers or views tie the queue to anything.
  for (auto const table : {"queue_entries", "queue_history", "queue_schema"}) {
    INFO(table);
    CHECK(scalar(*conn, std::format("select count(*) from pragma_foreign_key_list('{}')", table)) == "0");
  }
  CHECK(scalar(*conn,
               "select count(*) from sqlite_master where type in ('trigger', 'view') and sql like '%queue\\_%' escape '\\'") ==
        "0");

  // The marker holds exactly (1, 1).
  CHECK(scalar(*conn, "select group_concat(version || ',' || compat, ';') from queue_schema") == "1,1");

  // The sequence floor: the counter sits at 1000000 and the first entry is 1000001.
  CHECK(scalar(*conn, "select group_concat(seq) from sqlite_sequence where name = 'queue_entries'") == "1000000");
  CHECK(insert_queue_entry(*conn) == 1'000'001);
  CHECK(insert_queue_entry(*conn) == 1'000'002);

  // The CHECKs refuse what they should and admit the states the engine writes.
  CHECK_FALSE(
      conn->execute("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, enqueued_at, refreshed_mono) "
                    "values ('done', 'h', 1, 1, '/', '[]', 0, 0)")
          .has_value());
  CHECK_FALSE(conn->execute("update queue_entries set terminate_reason = 'killed'").has_value());
  CHECK(conn->execute("update queue_entries set state = 'running', terminate_reason = 'cancelled'").has_value());
  CHECK(conn->execute("insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
                      "values (1, 'abandoned', '/', '[]', 0, 0, 0)")
            .has_value());
  CHECK_FALSE(conn->execute("insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
                            "values (2, 'vanished', '/', '[]', 0, 0, 0)")
                  .has_value());
}

TEST_CASE("migration 40's down refuses by name while queue_entries has a row, and otherwise drops the three tables and the "
          "sequence row",
          "[db][migrate][queue][qp-migration]") {
  // Test-spec scenario "Error -- the down migration refuses while entries
  // exist" (plan 1089, task qp-migration).
  scratch_db_path scratch;
  auto            conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));

  auto const chain = planar::db::migrations();
  REQUIRE(chain.size() == 41);
  REQUIRE(chain[39].version_ == 40);
  REQUIRE(chain[39].name_ == "host_queue");
  // Peel the annotation migration (00041) so migration 40 is the head under test.
  REQUIRE(conn->execute(chain[40].down_sql_));

  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global', 'p', 'p', 'active')"));
  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global', 'q', 'q', 'draft')"));
  insert_queue_entry(*conn);
  REQUIRE(conn->execute("insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
                        "values (7, 'exited', '/', '[]', 0, 0, 0)"));

  auto const schema_before = canonical_schema_dump(*conn);

  auto const refused = conn->execute(chain[39].down_sql_);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().message_.contains("m00040_down_refused_live_queue_entries"));
  // Every table is unchanged.
  CHECK(canonical_schema_dump(*conn) == schema_before);
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "40");
  CHECK(scalar(*conn, "select count(*) from queue_entries") == "1");
  CHECK(scalar(*conn, "select count(*) from queue_history") == "1");
  CHECK(scalar(*conn, "select count(*) from queue_schema") == "1");
  CHECK(scalar(*conn, "select seq from sqlite_sequence where name = 'queue_entries'") == "1000001");
  CHECK(scalar(*conn, "select count(*) from plans") == "2");

  // Drained: the down drops the three tables (history included) and the
  // sequence row, keeps every planning row, and records version 39.
  REQUIRE(conn->execute("delete from queue_entries"));
  REQUIRE(conn->execute(chain[39].down_sql_));
  CHECK_FALSE(table_exists(*conn, "queue_entries"));
  CHECK_FALSE(table_exists(*conn, "queue_history"));
  CHECK_FALSE(table_exists(*conn, "queue_schema"));
  CHECK(scalar(*conn, "select count(*) from sqlite_sequence where name = 'queue_entries'") == "0");
  CHECK(scalar(*conn, "select count(*) from plans") == "2");
  CHECK(scalar(*conn, "select max(version) from schema_migrations") == "39");

  // Re-applied, the counter starts again at the floor (the counter-reset
  // case migrations/README.md § Host-queue rollback recovery covers).
  REQUIRE(planar::db::apply_all(*conn));
  CHECK(scalar(*conn, "select seq from sqlite_sequence where name = 'queue_entries'") == "1000000");
  CHECK(insert_queue_entry(*conn) == 1'000'001);
}

TEST_CASE("the host-queue rollback recipe and the schema contract are documented", "[db][migrate][queue][qp-migration][docs]") {
  // The scenario's documentation half: migrations/README.md names the
  // refusal and the drain-first procedure, and docs/architecture.md's
  // schema contract names the three tables.
  std::filesystem::path const migrations_dir = PLANAR_MIGRATIONS_DIR;

  auto const readme = read_source_file(migrations_dir / "README.md");
  auto const recipe = markdown_section(readme, "### Host-queue rollback recovery");
  REQUIRE_FALSE(recipe.empty());
  CHECK(recipe.contains("m00040_down_refused_live_queue_entries"));
  CHECK(recipe.contains("planar-agent queue cancel"));
  CHECK(recipe.contains("drain"));
  CHECK(recipe.contains("queue_history"));
  CHECK(recipe.contains("scripts/queue-logs-after-reset.py"));

  auto const architecture = read_source_file(migrations_dir.parent_path() / "docs" / "architecture.md");
  auto const tables       = markdown_section(architecture, "### Application tables");
  REQUIRE_FALSE(tables.empty());
  auto const row = tables.find("| 0040 host queue |");
  REQUIRE(row != std::string::npos);
  auto const line = std::string_view(tables).substr(row, tables.find('\n', row) - row);
  CHECK(line.contains("`queue_entries`"));
  CHECK(line.contains("`queue_history`"));
  CHECK(line.contains("`queue_schema`"));
  CHECK(line.contains("1000000"));
}

// ---------------------------------------------------------------------------
// Plan 1089, task qp-queue-compat: every migration that names a queue table
// inserts a queue_schema row (tech spec 656 § Enforcement, "Marker
// presence"). The constant-equals-chain and fingerprint pins need the
// hostqueue engine and live in src/engine/hostqueue/schema.t.cpp.
// ---------------------------------------------------------------------------

namespace {

/// @brief `NNNNN_name` of every migration in `chain` whose up SQL names
/// `queue_entries`, `queue_history` or `queue_schema` but inserts no
/// `queue_schema` row. Comments are ignored, so only what the SQL DOES counts.
/// @param chain A migration chain.
/// @return The offending migrations, in chain order; empty when every one complies.
auto migrations_without_queue_marker(std::span<planar::db::migration_record const> chain) -> std::vector<std::string> {
  std::regex const         names_queue_table(R"(\b(queue_entries|queue_history|queue_schema)\b)", std::regex::icase);
  std::regex const         inserts_marker(R"(\binsert\s+(or\s+\w+\s+)?into\s+queue_schema\b)", std::regex::icase);
  std::vector<std::string> out;
  for (auto const& record : chain) {
    auto const sql = sql_without_comments(record.up_sql_);
    if (std::regex_search(sql, names_queue_table) && !std::regex_search(sql, inserts_marker)) {
      out.push_back(std::format("{:05}_{}", record.version_, record.name_));
    }
  }
  return out;
}

} // namespace

TEST_CASE("every main migration that names a queue table inserts a queue_schema row",
          "[db][migrations][queue][qp-queue-compat]") {
  auto const chain = planar::db::migrations();
  CHECK(migrations_without_queue_marker(chain).empty());

  // Non-vacuity: the scan does see the migration that creates the tables.
  std::regex const names_queue_table(R"(\bqueue_entries\b)");
  CHECK(std::ranges::any_of(
      chain, [&](auto const& r) { return std::regex_search(sql_without_comments(r.up_sql_), names_queue_table); }));
}

TEST_CASE("a synthetic migration that alters queue_history without a queue_schema row fails the marker scan, named",
          "[db][migrations][queue][qp-queue-compat]") {
  // Test-spec scenario "Edge -- a migration touching the queue tables
  // without a marker row fails the chain test".
  auto const        embedded = planar::db::migrations();
  auto const        next     = embedded.back().version_ + 1;
  std::string const up       = std::format("alter table queue_history add column x integer;\n"
                                           "insert into schema_migrations (version, description) values ({}, 'x');\n",
                                           next);
  std::vector<planar::db::migration_record> chain(embedded.begin(), embedded.end());
  chain.push_back(planar::db::migration_record{.version_ = next, .name_ = "queue_history_x", .up_sql_ = up, .down_sql_ = ""});

  auto const offending = migrations_without_queue_marker(chain);
  REQUIRE(offending.size() == 1);
  CHECK(offending.front() == std::format("{:05}_queue_history_x", next));

  // A comment that merely mentions a queue table does not count, and the
  // same change WITH its marker row passes.
  std::string const commented = std::format("-- touches nothing in queue_entries\n"
                                            "insert into schema_migrations (version, description) values ({}, 'x');\n",
                                            next);
  chain.back().up_sql_        = commented;
  CHECK(migrations_without_queue_marker(chain).empty());
  std::string const marked = up + "insert into queue_schema (version, compat, description) values (2, 1, 'x');\n";
  chain.back().up_sql_     = marked;
  CHECK(migrations_without_queue_marker(chain).empty());
}
