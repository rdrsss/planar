// @file sessioncommits.t.cpp
// @brief Unit tests for `planar.engine.runtime.sessioncommits` (plan 996,
// task 6262).
//
// The module has ONE function and these tests pin the three things about it
// that are not readable off its signature:
//
//   * THE SORT COLUMN IS `recorded_at`, AND THE RENDERED COLUMN IS NOT.
//     `audit trail` displays `committed_at` when present. The fixture below
//     therefore writes two rows whose `recorded_at` and `committed_at`
//     orders DISAGREE, so an implementation that sorted by the column the
//     operator sees fails here rather than looking right in a hand-check.
//   * AN EMPTY `session_ids` IS AN ANSWER, NOT AN ERROR. Asserted against a
//     connection whose table HAS rows, so an implementation that ignored
//     the predicate would return those rows and fail here.
//
//     READ THIS BEFORE STRENGTHENING THAT CASE. The first draft of it
//     claimed to pin the early-return guard, on the reasoning that the
//     `in ()` it would otherwise compose is a SQLite syntax error. IT IS
//     NOT — SQLite accepts an empty `IN ()` list and evaluates it to false
//     (verified directly: `select count(*) from t where a in ()` returns 0
//     at exit 0). The break-probe that deletes the guard is therefore a
//     SURVIVOR, and is recorded as one rather than being made to look like
//     a kill. What this case pins is the CONTRACT (empty in, empty out, no
//     failure), which a mutant returning `query_failed` for the empty case
//     does kill — that probe was run and is a kill.
//   * `limit` CAPS AFTER THE SORT, so a cap of 1 returns the newest-recorded
//     row rather than an arbitrary one. Asserted by identity, not by count.
//
// Rows go in as raw SQL because nothing in this tree WRITES
// `session_commits`: the git-subprocess half of the oracle module is
// deferred (see sessioncommits.cppm), so there is no verb to seed through.
// This is the same posture audit_trail.t.cpp takes toward `audit_log`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.sessioncommits;

namespace {

namespace sc = planar::engine::runtime::sessioncommits;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_sessioncommits_test_{}_{}.db",
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

/// @brief Two sessions; three commits whose `recorded_at` order is the
/// REVERSE of their `committed_at` order.
///
/// sha `early-rec` was committed LAST but recorded FIRST, and `late-rec`
/// the other way round. Any implementation sorting on `committed_at`
/// returns them in the opposite order to the one asserted below.
auto seed(planar::db::connection& conn) -> void {
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into sessions (id, vendor, started_at) values (2, 'codex', '2026-01-01T00:00:00.000Z')");

  exec(conn, "insert into session_commits (id, session_id, sha, subject, committed_at, recorded_at) "
             "values (1, 1, 'early-rec', 'committed last, recorded first', "
             "'2026-03-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into session_commits (id, session_id, sha, subject, committed_at, recorded_at) "
             "values (2, 1, 'late-rec', 'committed first, recorded last', "
             "'2026-02-01T00:00:00.000Z', '2026-02-01T00:00:00.000Z')");
  // Belongs to the OTHER session, so a query that ignored its `in (…)`
  // predicate would pick it up.
  exec(conn, "insert into session_commits (id, session_id, sha, recorded_at) "
             "values (3, 2, 'other-session', '2026-04-01T00:00:00.000Z')");
}

} // namespace

TEST_CASE("list_for_sessions orders by recorded_at, not by the committed_at it renders", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  std::array<std::int64_t, 1> ids{1};
  auto const                  rows = sc::list_for_sessions(conn, ids, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  // Newest RECORDED first. `late-rec` has the later `recorded_at` and the
  // EARLIER `committed_at`; a committed_at sort would put `early-rec` here.
  CHECK((*rows)[0].sha == "late-rec");
  CHECK((*rows)[1].sha == "early-rec");
  // The other session's row is absent — the predicate is doing work.
  CHECK((*rows)[0].session_id == 1);
  CHECK((*rows)[1].session_id == 1);
}

TEST_CASE("list_for_sessions caps after sorting, so limit 1 keeps the newest-recorded row", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  std::array<std::int64_t, 1> ids{1};
  auto const                  rows = sc::list_for_sessions(conn, ids, 1);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  // By identity, not by count: a cap applied before the sort would also
  // return exactly one row.
  CHECK((*rows)[0].sha == "late-rec");
}

TEST_CASE("list_for_sessions spans every id it is given", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  std::array<std::int64_t, 2> ids{1, 2};
  auto const                  rows = sc::list_for_sessions(conn, ids, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 3);
  // Session 2's row was recorded last, so it leads.
  CHECK((*rows)[0].sha == "other-session");
  CHECK((*rows)[1].sha == "late-rec");
  CHECK((*rows)[2].sha == "early-rec");
}

TEST_CASE("list_for_sessions answers an empty id list without running a query", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The table HAS rows here on purpose: an implementation that ignored the
  // id predicate would return them and fail. It does NOT discriminate the
  // early-return guard — see this file's header for why that probe survives
  // and why it is reported rather than hidden.
  std::array<std::int64_t, 0> none{};
  auto const                  rows = sc::list_for_sessions(conn, none, std::nullopt);
  REQUIRE(rows.has_value());
  CHECK(rows->empty());
}

TEST_CASE("list_for_sessions returns every optional column, set and unset", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, status, "
             "vendor, claimed_at, last_heartbeat_at, lease_expires_at) "
             "values (7, 'tok', 1, 'task', 1, 'exclusive', 'active', 'claude', "
             "'2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', '2026-01-01T01:00:00.000Z')");
  exec(conn, "insert into session_commits (id, session_id, claim_id, sha, repo_root, branch, subject, author, "
             "committed_at, recorded_at) values (1, 1, 7, 'full', '/repo', 'main', 'subject text', 'Ada', "
             "'2026-02-01T00:00:00.000Z', '2026-02-01T00:00:01.000Z')");
  exec(conn, "insert into session_commits (id, session_id, sha, recorded_at) "
             "values (2, 1, 'bare', '2026-01-01T00:00:00.000Z')");

  std::array<std::int64_t, 1> ids{1};
  auto const                  rows = sc::list_for_sessions(conn, ids, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);

  // ABSENCE IS ASSERTED ALONGSIDE PRESENCE. A decode that hard-coded every
  // optional to `nullopt` would satisfy the `bare` row alone, so the `full`
  // row pins the other side of each field.
  auto const& full = (*rows)[0];
  CHECK(full.sha == "full");
  CHECK(full.claim_id == 7);
  CHECK(full.repo_root == "/repo");
  CHECK(full.branch == "main");
  CHECK(full.subject == "subject text");
  CHECK(full.author == "Ada");
  CHECK(full.committed_at == "2026-02-01T00:00:00.000Z");
  CHECK(full.recorded_at == "2026-02-01T00:00:01.000Z");

  auto const& bare = (*rows)[1];
  CHECK(bare.sha == "bare");
  CHECK_FALSE(bare.claim_id.has_value());
  CHECK_FALSE(bare.repo_root.has_value());
  CHECK_FALSE(bare.branch.has_value());
  CHECK_FALSE(bare.subject.has_value());
  CHECK_FALSE(bare.author.has_value());
  CHECK_FALSE(bare.committed_at.has_value());
  CHECK(bare.recorded_at == "2026-01-01T00:00:00.000Z");
}
