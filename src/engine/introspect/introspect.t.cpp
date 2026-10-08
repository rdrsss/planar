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
import planar.introspection_preview;

namespace {

namespace intro = planar::engine::introspect;
namespace ip    = planar::introspection_preview;

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

/// @brief A catalog predicate that recognises every path (the pre-mask behaviour).
auto const accept_all = [](std::string_view) { return true; };

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

} // namespace

TEST_CASE("build: empty database — all aggregates are zero / empty", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
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

TEST_CASE("build: health_summary fails CLOSED — a broken `tasks` query reports degraded, not ok",
          "[engine][introspect][build][health]") {
  // introspect.cppm's header on `health_summary` pins this as the one
  // fallback in the file that must NOT default to 0: a query failure on
  // `inflight` has to report "degraded" directly, because falling through
  // to `not_resumable = inflight - resumable` on a fabricated `0` would
  // read as "healthy" — exactly backwards for a health signal. This test
  // creates the actual failure condition (renaming `tasks` out from under
  // the query, so `conn.prepare(...)` genuinely fails with "no such
  // table") rather than deleting or weakening the guard.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "alter table tasks rename to tasks_renamed_away");

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());
  CHECK(b->health == "degraded");
}

TEST_CASE("build: logging disabled — invocations/failures are empty", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");

  auto b = intro::build(conn, 30, 20, false, "test-version", accept_all);
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

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
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

  auto b = intro::build(conn, 7, 20, true, "test-version", accept_all);
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

  auto b = intro::build(conn, 30, 2, true, "test-version", accept_all);
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

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  auto const text = intro::render_text(*b);
  CHECK(text.find(k_sentinel) == std::string::npos);

  auto const json = intro::render_json(*b);
  CHECK(json.find(k_sentinel) == std::string::npos);
}

TEST_CASE("build/cli_preview_jsonl: a predicate that accepts everything passes stored verb_path text through verbatim",
          "[engine][introspect][build][redaction]") {
  // The catalog predicate is the ONLY thing standing between a stored
  // `verb_path` and the bundle (the masking cases below); this case pins
  // that an accepting predicate changes nothing. introspect.cppm's header narrows the redaction claim to "no entity-TABLE
  // column is read" — it does NOT claim `verb_path` is free of
  // operator-authored text. `search` is a top-level verb with a REQUIRED
  // free-text positional (handlers/search.zig:38), so `cli_log.zig` records
  // `verb_path = "search <query>"` with the query inline, and this module
  // selects that column verbatim in three places. This test PINS that
  // leaking behavior (oracle-faithful, `cli_log.zig` is the writer and out
  // of scope here) so the next reader sees it directly instead of trusting
  // a redaction claim that doesn't hold for this one column.
  constexpr std::string_view k_sentinel = "SENTINEL_LEAKS";
  scratch_db_path            scratch;
  auto                       conn = open_migrated(scratch);

  exec(conn, std::format("insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
                         " values ('search {}', '<pos:1>', 2, 'usage', datetime('now'))",
                         k_sentinel));

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  // [invocations]: the free-text query is IN the rendered verb_path.
  REQUIRE(b->invocations.size() == 1);
  CHECK(b->invocations[0].verb_path.find(k_sentinel) != std::string::npos);
  CHECK(intro::render_text(*b).find(k_sentinel) != std::string::npos);
  CHECK(intro::render_json(*b).find(k_sentinel) != std::string::npos);

  // [failure tail]: same column, same leak.
  REQUIRE(b->failure_tail.size() == 1);
  CHECK(b->failure_tail[0].verb_path.find(k_sentinel) != std::string::npos);

  // The JSONL preview boundary leaks it too.
  auto preview = intro::cli_preview_jsonl(conn, 30, 4096, accept_all);
  REQUIRE(preview.has_value());
  CHECK(preview->jsonl.find(k_sentinel) != std::string::npos);
}

TEST_CASE("build/cli_preview_jsonl: a verb_path the predicate rejects is rendered <unrecognized> everywhere, rows kept",
          "[engine][introspect][build][redaction]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Historical leaked rows plus rows the writer produces today.
  for (std::string_view const path :
       {"search NDJSON", "bogusverb", "link task:7333", "task add", "<unknown>", "task <unknown>"}) {
    exec(conn, std::format("insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
                           " values ('{}', '--secret-term', 2, 'usage', datetime('now'))",
                           path));
  }

  auto const catalog = [](std::string_view path) {
    return path == "task add" || path == "<unknown>" || path == "task <unknown>";
  };

  auto b = intro::build(conn, 30, 20, true, "test-version", catalog);
  REQUIRE(b.has_value());

  // [invocations]: the three rejected paths aggregate into ONE masked row;
  // recognized paths, including the writer's own `<unknown>`, pass unchanged.
  std::map<std::string, std::int64_t> counts;
  for (auto const& v : b->invocations) {
    counts[v.verb_path] += v.count;
  }
  CHECK(b->invocations.size() == 4);
  CHECK(counts["<unrecognized>"] == 3);
  CHECK(counts["task add"] == 1);
  CHECK(counts["<unknown>"] == 1);
  CHECK(counts["task <unknown>"] == 1);
  for (auto const& v : b->invocations) {
    if (v.verb_path == "<unrecognized>") {
      CHECK(v.failure_count == 3);
    }
  }

  // [failure tail] masks per row and keeps every row.
  REQUIRE(b->failure_tail.size() == 6);
  std::int64_t masked_tail = 0;
  for (auto const& r : b->failure_tail) {
    masked_tail += r.verb_path == "<unrecognized>" ? 1 : 0;
  }
  CHECK(masked_tail == 3);

  auto const text = intro::render_text(*b);
  auto const json = intro::render_json(*b);
  for (auto const* leaked : {"NDJSON", "bogusverb", "task:7333", "secret-term"}) {
    CHECK(text.find(leaked) == std::string::npos);
    CHECK(json.find(leaked) == std::string::npos);
  }
  CHECK(text.find("<unrecognized>") != std::string::npos);
  CHECK(json.find("<unrecognized>") != std::string::npos);

  // The JSONL boundary masks too, and still carries every row.
  auto preview = intro::cli_preview_jsonl(conn, 30, 1 << 16, catalog);
  REQUIRE(preview.has_value());
  CHECK(preview->rows == 6);
  CHECK(preview->jsonl.find("NDJSON") == std::string::npos);
  CHECK(preview->jsonl.find("bogusverb") == std::string::npos);
  CHECK(preview->jsonl.find("task:7333") == std::string::npos);
  CHECK(preview->jsonl.find(R"("verb_path":"planar <unrecognized>")") != std::string::npos);
  CHECK(preview->jsonl.find(R"("verb_path":"planar task add")") != std::string::npos);
  CHECK(preview->jsonl.find(R"("verb_path":"planar <unknown>")") != std::string::npos);

  // No purge: the stored rows are untouched.
  auto stmt = conn.prepare("select count(*), sum(verb_path = 'search NDJSON') from cli_invocations");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 6);
  CHECK(stmt->column_int64(1) == 1);
}

TEST_CASE("build/cli_preview_jsonl: a legacy planar-prefixed stored path is judged bare at all three sites",
          "[engine][introspect][build][redaction]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('planar plan show', '', 2, 'usage', datetime('now'))");

  auto const catalog = [](std::string_view path) { return path == "plan show"; };

  auto b = intro::build(conn, 30, 20, true, "test-version", catalog);
  REQUIRE(b.has_value());
  REQUIRE(b->invocations.size() == 1);
  CHECK(b->invocations[0].verb_path == "planar plan show");
  REQUIRE(b->failure_tail.size() == 1);
  CHECK(b->failure_tail[0].verb_path == "planar plan show");

  auto preview = intro::cli_preview_jsonl(conn, 30, 4096, catalog);
  REQUIRE(preview.has_value());
  CHECK(preview->jsonl.find(R"("verb_path":"planar plan show")") != std::string::npos);
  CHECK(preview->jsonl.find("<unrecognized>") == std::string::npos);
}

TEST_CASE("build: merged masked rows sum every count and sort by the merged total", "[engine][introspect][build][redaction]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Three rejected paths, one row each (one succeeded), merge to a total of
  // 3 and must outrank the recognised path's 2 rows even though no single
  // rejected row does.
  for (std::string_view const row : {"'bogus1', 2", "'bogus2', 2", "'search A', 0", "'task add', 0", "'task add', 0"}) {
    exec(conn, std::format("insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
                           " values ({}, '', {}, datetime('now'))",
                           row.substr(0, row.find(',')), row.substr(row.find(',') + 2) == "0" ? "0, null" : "2, 'usage'"));
  }

  auto b = intro::build(conn, 30, 20, true, "test-version", [](std::string_view path) { return path == "task add"; });
  REQUIRE(b.has_value());
  REQUIRE(b->invocations.size() == 2);
  CHECK(b->invocations.front().verb_path == "<unrecognized>");
  CHECK(b->invocations.front().count == 3);
  CHECK(b->invocations.front().success_count == 1);
  CHECK(b->invocations.front().failure_count == 2);
  CHECK(b->invocations.back().verb_path == "task add");
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

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  CHECK(b->handoffs.stale_handoffs == 0);
  CHECK(b->handoffs.never_consumed == 2);
  CHECK(b->handoffs.stale_handoffs != b->handoffs.never_consumed);
}

TEST_CASE("build: reopens count reflects seeded task_reopens rows", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  {
    auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
    REQUIRE(b.has_value());
    CHECK(b->reopens == 0);
  }

  exec(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'done', 100)");
  exec(conn, "insert into task_reopens (task_id, from_status, to_status, source) values (1, 'done', 'todo', 'task-reopen')");
  exec(conn,
       "insert into task_reopens (task_id, from_status, to_status, source) values (1, 'done', 'doing', 'task-update-force')");

  auto b2 = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b2.has_value());
  CHECK(b2->reopens == 2);

  // A 1-day window still includes these rows: all timestamps are datetime('now').
  auto b3 = intro::build(conn, 1, 20, true, "test-version", accept_all);
  REQUIRE(b3.has_value());
  CHECK(b3->reopens == 2);
}

TEST_CASE("build: schema_version reflects the applied migrations", "[engine][introspect][build]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  auto stmt = conn.prepare("select max(version) from schema_migrations");
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  CHECK(b->schema_version == stmt->column_int64(0));
}

TEST_CASE("build: agent_actions aggregates a populated table, not just the empty-database fallback",
          "[engine][introspect][build][actions]") {
  // The prior suite only ever exercised query_action_outcomes' EMPTY path
  // (empty database) and its prepare-failure fallback (never reachable in
  // a migrated DB). Neither distinguishes the loop's normal-completion
  // `return rows;` from a hypothetical `return {};` — both look identical
  // on zero rows. This seeds real rows so a populated result is pinned.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into sessions (vendor, started_at) values ('claude', datetime('now'))");
  exec(conn, "insert into agent_actions (session_id, action_kind, vendor, outcome, started_at)"
             " values (1, 'coder', 'claude', 'ok', datetime('now'))");
  exec(conn, "insert into agent_actions (session_id, action_kind, vendor, outcome, started_at)"
             " values (1, 'coder', 'claude', 'ok', datetime('now'))");
  exec(conn, "insert into agent_actions (session_id, action_kind, vendor, outcome, started_at)"
             " values (1, 'reviewer', 'claude', 'error', datetime('now'))");

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  REQUIRE(b->actions.size() == 2);
  CHECK(b->actions[0].action_kind == "coder");
  CHECK(b->actions[0].outcome == "ok");
  CHECK(b->actions[0].count == 2);
  CHECK(b->actions[1].action_kind == "reviewer");
  CHECK(b->actions[1].outcome == "error");
  CHECK(b->actions[1].count == 1);

  // render_text's "  {}: total={} ok={} fail={}\n" invocations format is
  // otherwise unpinned, and its own populated-row test is separate below
  // (build: cli_invocations render_text/render_json pin the exact
  // populated-row format); this test's job is the actions AGGREGATE, which
  // renders through the "{}/{}: {}\n" action line instead.
  auto const text = intro::render_text(*b);
  CHECK(text.find("[actions]\n  coder/ok: 2\n  reviewer/error: 1\n") != std::string::npos);
}

TEST_CASE("build+render: a populated invocations bundle pins the exact render_text and render_json shapes",
          "[engine][introspect][build][render]") {
  // Closes the gap the reviewer found: render_text is ~80 lines / ~14
  // branches with exactly one prior positive assertion in the whole suite
  // (the "unavailable" preview line), and no test rendered JSON from a
  // populated bundle at all. This pins BOTH renderers' exact populated-row
  // output for `invocations`, so a format-string mutation like
  // `:394`'s `"  {}: total={} ok={} fail={}\n"` is caught.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('health', '', 0, datetime('now'))");
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('task add', '<pos:1>', 2, 'usage', datetime('now'))");

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  auto const text = intro::render_text(*b);
  CHECK(text.find("[invocations]\n  health: total=2 ok=2 fail=0\n  task add: total=1 ok=0 fail=1\n") != std::string::npos);
  CHECK(text.find("[failures]\n  usage: 1\n") != std::string::npos);

  auto const json = intro::render_json(*b);
  CHECK(json.find(R"("invocations":[{"verb_path":"health","count":2,"success_count":2,"failure_count":0},)"
                  R"({"verb_path":"task add","count":1,"success_count":0,"failure_count":1}])") != std::string::npos);
  CHECK(json.find(R"("failures":[{"category":"usage","count":1}])") != std::string::npos);
}

TEST_CASE("build+render: claims — stale_claims and never_consumed are NOT interchangeable in render_json",
          "[engine][introspect][build][render][claims]") {
  // Guards render_json's `",\"claims\":{{\"stale_claims\":{},\"never_consumed\":{}}}"`
  // (:545) against a field swap: seeds deliberately DIFFERENT counts (3
  // stale, 1 never-consumed) so `{"stale_claims":1,...}` and
  // `{"stale_claims":3,...}` cannot both satisfy the same substring check.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into sessions (vendor, started_at) values ('claude', datetime('now'))");
  // Three ACTIVE claims older than 24h -> stale_claims = 3.
  for (int i = 0; i < 3; ++i) {
    exec(conn, std::format("insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor,"
                           " claimed_at, lease_expires_at)"
                           " values ('tok-stale-{}', 1, 'task', 1, 'active', 'claude',"
                           " datetime('now','-2 days'), datetime('now','+1 hour'))",
                           i));
  }
  // One EXPIRED/RELEASED claim -> never_consumed = 1.
  exec(conn, "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor,"
             " claimed_at, lease_expires_at)"
             " values ('tok-expired', 1, 'task', 1, 'released', 'claude',"
             " datetime('now'), datetime('now','+1 hour'))");

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());
  REQUIRE(b->claims.stale_claims == 3);
  REQUIRE(b->claims.never_consumed == 1);

  auto const json = intro::render_json(*b);
  CHECK(json.find(R"("claims":{"stale_claims":3,"never_consumed":1})") != std::string::npos);
  CHECK(json.find(R"("claims":{"stale_claims":1,"never_consumed":3})") == std::string::npos);

  auto const text = intro::render_text(*b);
  CHECK(text.find("[claims]        stale=3 never_consumed=1\n") != std::string::npos);
}

TEST_CASE("build: claim_failure_category_count — provider and category are NOT interchangeable",
          "[engine][introspect][build][claims]") {
  // Guards query_claim_failure_categories' row construction
  // (`.provider = stmt->column_text(0), .category = stmt->column_text(1)`,
  // :287-288) against a column swap: `vendor` and `failure_category` are
  // seeded with values that look nothing alike, so a swap is visible on
  // either field alone.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into sessions (vendor, started_at) values ('claude', datetime('now'))");
  exec(conn, "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor,"
             " failure_category, claimed_at, lease_expires_at)"
             " values ('tok-1', 1, 'task', 1, 'aborted', 'claude-provider-marker',"
             " 'tool_failure', datetime('now'), datetime('now','+1 hour'))");

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());

  REQUIRE(b->claim_failure_categories.size() == 1);
  CHECK(b->claim_failure_categories[0].provider == "claude-provider-marker");
  CHECK(b->claim_failure_categories[0].category == "tool_failure");

  auto const json = intro::render_json(*b);
  CHECK(json.find(R"("claim_failure_categories":[{"provider":"claude-provider-marker","category":"tool_failure","count":1}])") !=
        std::string::npos);
}

TEST_CASE("cli_preview_jsonl: exceeding max_bytes keeps the newest rows, oldest first, and counts the omitted ones",
          "[engine][introspect][cli_preview_jsonl]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Row r0 is the newest (now), r9 the oldest (nine minutes back).
  for (int i = 0; i < 10; ++i) {
    exec(conn, std::format("insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
                           " values ('r{}', '', 0, datetime('now', '-{} minutes'))",
                           i, i));
  }

  auto const full = intro::cli_preview_jsonl(conn, 30, 1 << 20, accept_all);
  REQUIRE(full.has_value());
  REQUIRE(full->rows == 10);
  CHECK_FALSE(full->truncated);
  CHECK(full->omitted == 0);
  auto const row_size = full->jsonl.size() / 10;
  REQUIRE(row_size * 10 == full->jsonl.size());

  // Room for exactly four rows (plus a partial fifth): the four newest stay.
  auto preview = intro::cli_preview_jsonl(conn, 30, row_size * 4 + row_size / 2, accept_all);
  REQUIRE(preview.has_value());
  CHECK(preview->truncated);
  CHECK(preview->rows == 4);
  CHECK(preview->omitted == 6);
  CHECK(preview->jsonl.size() <= row_size * 4 + row_size / 2);
  auto const p3 = preview->jsonl.find("planar r3");
  auto const p2 = preview->jsonl.find("planar r2");
  auto const p1 = preview->jsonl.find("planar r1");
  auto const p0 = preview->jsonl.find("planar r0");
  REQUIRE(p3 != std::string::npos);
  REQUIRE(p0 != std::string::npos);
  CHECK(p3 < p2);
  CHECK(p2 < p1);
  CHECK(p1 < p0);
  CHECK(preview->jsonl.find("planar r4") == std::string::npos);
  CHECK(preview->jsonl.find("planar r9") == std::string::npos);

  // A budget smaller than any one row reads nothing and omits everything.
  auto none = intro::cli_preview_jsonl(conn, 30, 16, accept_all);
  REQUIRE(none.has_value());
  CHECK(none->jsonl.empty());
  CHECK(none->truncated);
  CHECK(none->omitted == 10);
}

TEST_CASE("cli_preview_jsonl: a window exactly at max_bytes is not truncated; one more row omits exactly one",
          "[engine][introspect][cli_preview_jsonl]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  for (int i = 0; i < 5; ++i) {
    exec(conn, std::format("insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
                           " values ('r{}', '', 0, datetime('now', '-{} minutes'))",
                           i, i));
  }
  auto const full = intro::cli_preview_jsonl(conn, 30, 1 << 20, accept_all);
  REQUIRE(full.has_value());
  auto const exact = full->jsonl.size();

  auto at_cap = intro::cli_preview_jsonl(conn, 30, exact, accept_all);
  REQUIRE(at_cap.has_value());
  CHECK_FALSE(at_cap->truncated);
  CHECK(at_cap->omitted == 0);
  CHECK(at_cap->rows == 5);
  CHECK(at_cap->jsonl == full->jsonl);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)"
             " values ('r5', '', 0, datetime('now', '-6 minutes'))");
  auto over = intro::cli_preview_jsonl(conn, 30, exact, accept_all);
  REQUIRE(over.has_value());
  CHECK(over->truncated);
  CHECK(over->omitted == 1);
  CHECK(over->rows == 5);
  CHECK(over->jsonl.find("planar r5") == std::string::npos);
}

TEST_CASE("render_json: failure_tail and logging_enabled are present, and version is what build was given",
          "[engine][introspect][render]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('task add', '', 2, 'usage', datetime('now'))");

  auto on = intro::build(conn, 30, 20, true, "abc123def456", accept_all);
  REQUIRE(on.has_value());
  auto const on_json = intro::render_json(*on);
  CHECK(on_json.starts_with(R"({"version":"abc123def456",)"));
  CHECK(on_json.find(R"("logging_enabled":true)") != std::string::npos);
  CHECK(on_json.find(R"("failure_tail":[{"verb_path":"task add","error_category":"usage","exit_code":2,"recorded_at":")") !=
        std::string::npos);

  auto off = intro::build(conn, 30, 20, false, "abc123def456", accept_all);
  REQUIRE(off.has_value());
  auto const off_json = intro::render_json(*off);
  CHECK(off_json.find(R"("logging_enabled":false)") != std::string::npos);
  CHECK(off_json.find(R"("failure_tail":[])") != std::string::npos);
}

TEST_CASE("cli_preview_jsonl: canonicalizes captured verb paths exactly once", "[engine][introspect][cli_preview_jsonl]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)"
             " values ('task add', '', 2, 'usage', strftime('%Y-%m-%dT%H:%M:%SZ','now')),"
             "        ('planar plan show', '', 1, 'not_found', datetime('now'))");

  auto preview = intro::cli_preview_jsonl(conn, 30, 4096, accept_all);
  REQUIRE(preview.has_value());
  CHECK(preview->jsonl.find("\"verb_path\":\"planar task add\"") != std::string::npos);
  CHECK(preview->jsonl.find("\"verb_path\":\"planar plan show\"") != std::string::npos);
  CHECK(preview->jsonl.find("planar planar") == std::string::npos);
  CHECK(preview->jsonl.find("ZZ") == std::string::npos);
}

TEST_CASE("cli_preview_jsonl: empty window still returns an empty (not null) result", "[engine][introspect][cli_preview_jsonl]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto preview = intro::cli_preview_jsonl(conn, 30, 4096, accept_all);
  REQUIRE(preview.has_value());
  CHECK(preview->jsonl.empty());
  CHECK_FALSE(preview->truncated);
  CHECK(preview->omitted == 0);
}

TEST_CASE("render_json: empty windows emit empty arrays, never nulls", "[engine][introspect][render]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
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

TEST_CASE("render_text: introspection preview renders 'unavailable' when build() leaves it unset",
          "[engine][introspect][render]") {
  // `bundle` HAS carried a `preview` field since task 6352 (decision 981) —
  // this test's title used to say otherwise, citing D15 as a permanent
  // reason the field could not exist at all. D15 only forbade this
  // module reaching `engine_introspection_adapters` directly; decision
  // 981's layer-1 extraction closed that gap. What is still true, and
  // what this test actually pins, is that `build()` (DB-only) never
  // POPULATES the field — only the `report` handler does, by calling
  // `introspection_adapters::collect_preview_from_paths` separately. A
  // bare `build()` call therefore always renders "unavailable" here.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());
  REQUIRE_FALSE(b->preview.has_value());

  auto const text = intro::render_text(*b);
  CHECK(text.find("[introspection preview] unavailable\n") != std::string::npos);
}

TEST_CASE("render_text: a preview with no coverage rows renders 'empty', distinct from unset ('unavailable')",
          "[engine][introspect][render][preview]") {
  // The oracle distinguishes THREE preview render states, not two: unset
  // (`bundle.preview == null`, "unavailable"), present-but-no-coverage-rows
  // ("empty" — reachable only if a caller constructs an empty `preview` by
  // hand; `collect_preview_from_paths` always emits at least four coverage
  // rows in production), and populated (rendered in full, next test). This
  // pins the middle state so a future edit cannot silently collapse it
  // into either of the other two.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());
  b->preview = ip::preview{};
  REQUIRE(b->preview->coverage.empty());

  auto const text = intro::render_text(*b);
  CHECK(text.find("[introspection preview] empty\n") != std::string::npos);
  CHECK(text.find("unavailable") == std::string::npos);

  auto const json = intro::render_json(*b);
  CHECK(json.find(R"("introspection_preview":{"signals":[],"coverage":[],"warnings":[]})") != std::string::npos);
}

TEST_CASE("build+render: a populated preview renders every coverage/signal/warning row, in order, both formats",
          "[engine][introspect][render][preview]") {
  // No prior test exercised `render_text`/`render_json`'s POPULATED preview
  // branch at all (only "unset" and, above, "empty") — this closes that
  // gap and pins the exact oracle-captured shapes (zig:872-909,
  // 1013-1040): one coverage line per row, then one signal line, then one
  // warning line (text); one flat array per field, vendor/category/state/
  // kind rendered as their bare tag names, never numeric (JSON).
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto b = intro::build(conn, 30, 20, true, "test-version", accept_all);
  REQUIRE(b.has_value());
  b->preview = ip::preview{
      .signals =
          {
              ip::signal_row{
                  .v          = ip::vendor::claude,
                  .verb_path  = "planar task show",
                  .cat        = ip::category::failure,
                  .count      = 3,
                  .first_seen = "2026-07-12T12:00:00Z",
                  .last_seen  = "2026-07-12T12:05:00Z",
              },
          },
      .coverage =
          {
              ip::coverage_row{
                  .v          = ip::vendor::claude,
                  .state      = ip::coverage_state::observed,
                  .scanned    = 10,
                  .malformed  = 1,
                  .normalized = 3,
                  .ignored    = 6,
                  .capped     = 0,
              },
          },
      .warnings =
          {
              ip::warning_row{.v = ip::vendor::claude, .kind = ip::warning_kind::malformed, .count = 1},
          },
  };

  auto const text = intro::render_text(*b);
  CHECK(text.find("[introspection preview]\n"
                  "  claude: state=observed scanned=10 normalized=3 ignored=6 malformed=1 capped=0\n"
                  "  signal claude/planar task show/failure: count=3 first=2026-07-12T12:00:00Z last=2026-07-12T12:05:00Z\n"
                  "  warning claude/malformed: count=1\n") != std::string::npos);

  auto const json = intro::render_json(*b);
  CHECK(json.find(R"("introspection_preview":{"signals":[{"vendor":"claude","verb_path":"planar task show",)"
                  R"("category":"failure","count":3,"first_seen":"2026-07-12T12:00:00Z",)"
                  R"("last_seen":"2026-07-12T12:05:00Z"}],"coverage":[{"vendor":"claude","state":"observed",)"
                  R"("scanned":10,"malformed":1,"normalized":3,"capped":0,"ignored":6}],)"
                  R"("warnings":[{"vendor":"claude","kind":"malformed","count":1}]}})") != std::string::npos);
}
