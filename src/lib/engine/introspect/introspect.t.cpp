// @file introspect.t.cpp
// @brief Unit tests for `planar.engine.introspect` (plan 996, task 6121).
//
// Ported from the oracle's zig/src/engine/introspect.zig unit-test block,
// case for case — same fixtures, same assertions. Rows are inserted with
// raw SQL because the engine takes no writes.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.introspect;

namespace {

namespace intro = planar::engine::introspect;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_introspect_test_{}_{}.db",
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

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

} // namespace

TEST_CASE("build: empty database — all aggregates are zero / empty", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  CHECK(b->invocations.empty());
  CHECK(b->failures.empty());
  CHECK(b->actions.empty());
  CHECK(b->sync.empty());
  CHECK(b->claims.stale_claims == 0);
  CHECK(b->claim_failure_categories.empty());
  CHECK(b->handoffs.stale_handoffs == 0);
  CHECK(b->failure_tail.empty());
  CHECK(b->window_days == 30);
  CHECK(b->health == "ok");
}

TEST_CASE("build: logging disabled — invocations/failures are empty", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");

  auto b = intro::build(conn, 30, 20, false, "/tmp/test.db");
  REQUIRE(b.has_value());

  CHECK_FALSE(b->logging_enabled);
  CHECK(b->invocations.empty());
  CHECK(b->failures.empty());
  CHECK(b->failure_tail.empty());
}

TEST_CASE("build: aggregates reflect seeded cli_invocations", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('task add', '<pos:1>', 2, 'usage', datetime('now'))");

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  REQUIRE(b->invocations.size() == 2);
  CHECK(b->invocations[0].verb_path == "health");
  CHECK(b->invocations[0].count == 2);
  CHECK(b->invocations[0].success_count == 2);
  CHECK(b->invocations[0].failure_count == 0);

  REQUIRE(b->failures.size() == 1);
  CHECK(b->failures[0].category == "usage");
  CHECK(b->failures[0].count == 1);

  REQUIRE(b->failure_tail.size() == 1);
  CHECK(b->failure_tail[0].verb_path == "task add");
  CHECK(b->failure_tail[0].exit_code == 2);
}

TEST_CASE("build: window filter excludes old rows", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('recent', '', 0, datetime('now', '-5 days')),"
             "        ('old', '', 0, datetime('now', '-31 days'))");

  auto b = intro::build(conn, 7, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  REQUIRE(b->invocations.size() == 1);
  CHECK(b->invocations[0].verb_path == "recent");
}

TEST_CASE("build: failure tail cap — only tail_n rows returned", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('v1', '', 2, 'usage', datetime('now', '-4 minutes')),"
             "        ('v2', '', 2, 'usage', datetime('now', '-3 minutes')),"
             "        ('v3', '', 2, 'usage', datetime('now', '-2 minutes')),"
             "        ('v4', '', 2, 'usage', datetime('now', '-1 minutes')),"
             "        ('v5', '', 2, 'usage', datetime('now'))");

  auto b = intro::build(conn, 30, 2, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  REQUIRE(b->failure_tail.size() == 2);
  CHECK(b->failure_tail[0].verb_path == "v5");
  CHECK(b->failure_tail[1].verb_path == "v4");
}

TEST_CASE("build: redaction — seeded sentinel titles never appear in text or JSON output", "[engine][introspect][build]") {
  constexpr std::string_view k_sentinel = "SENTINEL_MUST_NOT_APPEAR_IN_REPORT";
  scratch_db_path            scratch;
  auto                       conn = open_migrated(scratch);

  exec(conn,
       std::format("insert into tasks (scope_kind, title, status, priority) values ('global', '{}', 'todo', 100)", k_sentinel));
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  auto const text = intro::render_text(*b);
  CHECK(text.find(k_sentinel) == std::string::npos);

  auto const json = intro::render_json(*b);
  CHECK(json.find(k_sentinel) == std::string::npos);
}

TEST_CASE("build: handoffs never_consumed is distinct from stale_handoffs", "[engine][introspect][build]") {
  // A consumed handoff created > 24h ago: stale_handoffs = 0 (already
  // consumed), never_consumed = 0 (was consumed). Two unconsumed (pending
  // and validated) handoffs created recently: stale_handoffs = 0 (not
  // old), never_consumed = 2. Proves the two fields can differ when data
  // is mixed, AND deliberately makes never_consumed (2) != the consumed
  // count (1) — a fixture with equal consumed/unconsumed counts cannot
  // discriminate `status != 'consumed'` from an accidental `status =
  // 'consumed'` flip, since both would report the same number.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into sessions (vendor, started_at) values ('test', datetime('now'))");
  exec(conn, "insert into context_snapshots (session_id, vendor, created_at) values (1, 'test', datetime('now'))");

  exec(conn, "insert into handoffs (from_snapshot_id, from_vendor, status, created_at, consumed_at)"
             " values (1, 'v1', 'consumed', datetime('now', '-2 days'), datetime('now', '-1 days'))");
  exec(conn, "insert into handoffs (from_snapshot_id, from_vendor, status, created_at)"
             " values (1, 'v1', 'pending', datetime('now'))");
  exec(conn, "insert into handoffs (from_snapshot_id, from_vendor, status, created_at)"
             " values (1, 'v1', 'validated', datetime('now'))");

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  CHECK(b->handoffs.stale_handoffs == 0);
  CHECK(b->handoffs.never_consumed == 2);
  CHECK(b->handoffs.stale_handoffs != b->handoffs.never_consumed);
}

TEST_CASE("build: reopens count reflects seeded task_reopens rows", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  {
    auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
    REQUIRE(b.has_value());
    CHECK(b->reopens == 0);
  }

  exec(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'done', 100)");
  exec(conn, "insert into task_reopens (task_id, from_status, to_status, source) values (1, 'done', 'todo', 'task-reopen')");
  exec(conn,
       "insert into task_reopens (task_id, from_status, to_status, source) values (1, 'done', 'doing', 'task-update-force')");

  auto b2 = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b2.has_value());
  CHECK(b2->reopens == 2);

  // A 1-day window still includes these rows: all timestamps are datetime('now').
  auto b3 = intro::build(conn, 1, 20, true, "/tmp/test.db");
  REQUIRE(b3.has_value());
  CHECK(b3->reopens == 2);
}

TEST_CASE("build: schema_version reflects the applied migrations", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  auto stmt = conn.prepare("select max(version) from schema_migrations");
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  CHECK(b->schema_version == stmt->column_int64(0));
}

TEST_CASE("cli_preview_jsonl: canonicalizes captured verb paths exactly once", "[engine][introspect][cli_preview_jsonl]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('task add', '', 2, 'usage', strftime('%Y-%m-%dT%H:%M:%SZ','now')),"
             "        ('planar plan show', '', 1, 'not_found', datetime('now'))");

  auto preview = intro::cli_preview_jsonl(conn, 30, 4096);
  REQUIRE(preview.has_value());
  CHECK(preview->find("\"verb_path\":\"planar task add\"") != std::string::npos);
  CHECK(preview->find("\"verb_path\":\"planar plan show\"") != std::string::npos);
  CHECK(preview->find("planar planar") == std::string::npos);
  CHECK(preview->find("ZZ") == std::string::npos);
}

TEST_CASE("cli_preview_jsonl: empty window still returns an empty (not null) result", "[engine][introspect][cli_preview_jsonl]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto preview = intro::cli_preview_jsonl(conn, 30, 4096);
  REQUIRE(preview.has_value());
  CHECK(preview->empty());
}

TEST_CASE("render_json: empty windows emit empty arrays, never nulls", "[engine][introspect][render]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  auto const json = intro::render_json(*b);
  CHECK(json.find("\"invocations\":[]") != std::string::npos);
  CHECK(json.find("\"failures\":[]") != std::string::npos);
  CHECK(json.find("\"actions\":[]") != std::string::npos);
  CHECK(json.find("\"sync\":[]") != std::string::npos);
  CHECK(json.find("\"claim_failure_categories\":[]") != std::string::npos);
  CHECK(json.find("\"introspection_preview\":{\"signals\":[],\"coverage\":[],\"warnings\":[]}") != std::string::npos);
  CHECK(json.find("null") == std::string::npos);
  CHECK(json.starts_with('{'));
  CHECK(json.ends_with("}\n"));
}

TEST_CASE("render_text: introspection preview renders 'unavailable' — bundle carries no preview field (D15)",
          "[engine][introspect][render]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "/tmp/test.db");
  REQUIRE(b.has_value());

  auto const text = intro::render_text(*b);
  CHECK(text.find("[introspection preview] unavailable\n") != std::string::npos);
}
