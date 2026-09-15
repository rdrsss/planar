// activity_rollup.t.cpp -- the three regimes of the per-entity rollup
// (task 6282, decision D19).
//
// Plain `//`, not `///`: test TUs in this tree do not carry Doxygen `@file`
// blocks.
//
// This module was extracted out of `engine_tree`, where it was a documented
// duplicate of the oracle's `summary.forEntity` kept because
// `engine_tree -> engine_runtime` is an edge the architecture guard refuses.
// The walker's own tests exercised the rollup only THROUGH a tree walk; these
// pin it directly, which is what a layer-1 primitive two buckets may call
// needs.
//
// The three regimes are distinct and each has its own failure mode:
//   - actions present  -> the newest action wins, and `ended_at` beats
//     `started_at`;
//   - no actions but a claim -> the claim-only fallback, whose action kind is
//     deliberately EMPTY (the text renderer prints the literal word `claim`);
//   - neither -> absent, which is NOT a zeroed summary.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.activity_rollup;
import planar.db;
import planar.db.migrate;

namespace ar = planar::activity_rollup;

namespace {

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_rollup_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)                    = delete;
  auto operator=(const scratch_db_path&) -> scratch_db_path& = delete;
  scratch_db_path(scratch_db_path&&)                         = delete;
  auto operator=(scratch_db_path&&) -> scratch_db_path&      = delete;

  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(std::filesystem::path{path_.string() + suffix}, ec);
    }
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
  auto done = conn.execute(sql);
  REQUIRE(done.has_value());
}

// `agent_actions.session_id` and `agent_work_claims.session_id` are both
// `not null references sessions(id)` with foreign keys ON. Seeding activity
// without this row silently rejects the insert and leaves the case asserting
// against an entity with no activity -- indistinguishable from a rollup that
// never populates anything. `exec` REQUIREs, so it fails here instead.
auto seed(planar::db::connection& conn) -> void {
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-02-01T00:00:00.000Z')");
  exec(conn, "insert into plans (id, scope_kind, scope_id, title, slug, status) "
             "values (1, 'global', null, 'Plan', 'plan', 'active')");
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, slug) "
             "values (7, 'global', null, 1, 'Task', 'todo', 'task-seven')");
}

} // namespace

TEST_CASE("an entity with neither actions nor claims has no rollup at all", "[activity_rollup][6282]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto got = ar::for_entity(conn, "task", 7);
  REQUIRE(got.has_value());
  // Absent, not a zeroed summary: renderers key off absence to omit the line
  // and the JSON key entirely.
  CHECK_FALSE(got->has_value());
}

TEST_CASE("the newest action wins, and ended_at beats started_at", "[activity_rollup][6282]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into agent_actions (id, session_id, entity_kind, entity_id, action_kind, vendor, started_at, ended_at) "
             "values (1, 1, 'task', 7, 'planner', 'codex', '2026-02-01T00:00:00.000Z', '2026-02-01T00:01:00.000Z')");
  // Ordered by `coalesce(ended_at, started_at) desc`: this row STARTED first
  // but ENDED last, so it wins. Ordering by started_at would pick the other.
  exec(conn, "insert into agent_actions (id, session_id, entity_kind, entity_id, action_kind, vendor, started_at, ended_at) "
             "values (2, 1, 'task', 7, 'coder', 'claude', '2026-01-01T00:00:00.000Z', '2026-02-01T00:05:00.000Z')");

  auto got = ar::for_entity(conn, "task", 7);
  REQUIRE(got.has_value());
  REQUIRE(got->has_value());
  CHECK((*got)->latest_action_kind == "coder");
  CHECK((*got)->latest_vendor == "claude");
  CHECK((*got)->last_event_at == "2026-02-01T00:05:00.000Z");
  // An action is not a claim.
  CHECK((*got)->active_claim_count == 0);
}

TEST_CASE("a claim with no action falls back to an EMPTY action kind", "[activity_rollup][6282]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  // Deliberately RELEASED: the fallback probes for ANY claim row, not an
  // active one, because a released claim is still activity worth surfacing.
  exec(conn, "insert into agent_work_claims (id, session_id, entity_kind, entity_id, claim_token, vendor, role, status, "
             "claimed_at, released_at, lease_expires_at) values (1, 1, 'task', 7, 'tok', 'codex', 'coder', 'released', "
             "'2026-02-01T00:00:00.000Z', '2026-02-01T00:09:00.000Z', '2026-02-01T00:30:00.000Z')");

  auto got = ar::for_entity(conn, "task", 7);
  REQUIRE(got.has_value());
  REQUIRE(got->has_value());
  // EMPTY, not "claim": the renderer supplies that word. A fallback that
  // filled in a kind here would change the rendered line.
  CHECK((*got)->latest_action_kind.empty());
  CHECK((*got)->latest_vendor == "codex");
  CHECK((*got)->last_event_at == "2026-02-01T00:09:00.000Z");
  // Released, so not counted as active.
  CHECK((*got)->active_claim_count == 0);
}

TEST_CASE("active_claim_count counts only unexpired active claims", "[activity_rollup][6282]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into agent_work_claims (id, session_id, entity_kind, entity_id, claim_token, vendor, role, status, "
             "claimed_at, lease_expires_at) values (1, 1, 'task', 7, 'live', 'claude', 'coder', 'active', "
             "'2026-02-01T00:00:00.000Z', '2999-01-01T00:00:00.000Z')");
  // Active but LAPSED: the lease matters, not the status alone.
  exec(conn, "insert into agent_work_claims (id, session_id, entity_kind, entity_id, claim_token, vendor, role, status, "
             "claimed_at, lease_expires_at) values (2, 1, 'task', 7, 'lapsed', 'claude', 'coder', 'active', "
             "'2026-02-01T00:00:00.000Z', '2000-01-01T00:00:00.000Z')");

  auto got = ar::for_entity(conn, "task", 7);
  REQUIRE(got.has_value());
  REQUIRE(got->has_value());
  CHECK((*got)->active_claim_count == 1);
}

TEST_CASE("the rollup is scoped to its own entity", "[activity_rollup][6282]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, slug) "
             "values (8, 'global', null, 1, 'Other', 'todo', 'task-eight')");
  exec(conn, "insert into agent_actions (id, session_id, entity_kind, entity_id, action_kind, vendor, started_at) "
             "values (1, 1, 'task', 8, 'coder', 'claude', '2026-02-01T00:00:00.000Z')");

  // Non-vacuity for the entity_id bind: task 8 HAS activity, so a rollup that
  // ignored the predicate would answer for task 7 too.
  auto seven = ar::for_entity(conn, "task", 7);
  REQUIRE(seven.has_value());
  CHECK_FALSE(seven->has_value());
  auto eight = ar::for_entity(conn, "task", 8);
  REQUIRE(eight.has_value());
  CHECK(eight->has_value());

  // And to its own KIND: a plan with the same numeric id is a different row.
  auto plan_seven = ar::for_entity(conn, "plan", 8);
  REQUIRE(plan_seven.has_value());
  CHECK_FALSE(plan_seven->has_value());
}
