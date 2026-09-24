// @file audit_trail.t.cpp
// @brief Unit tests for `planar.engine.runtime.audit_trail` (plan 996,
// task 6090).
//
// Three of this module's four functions are UNWIRED — only
// `session_timeline` has a leaf (`audit session`). That makes these tests
// the only thing standing behind `for_entity`, `for_entity_with_links` and
// `for_entity_grep` until `audit trail` is ported, so they pin the
// properties an `audit trail` implementer would otherwise have to
// rediscover:
//
//   * `for_entity_with_links` follows edges in BOTH directions and exactly
//     ONE hop. Both halves are asserted with a fixture that has a two-hop
//     chain, so a transitive-closure implementation FAILS rather than
//     merely returning more rows.
//   * `for_entity_grep`'s pattern is unescaped, so `%` and `_` in it are
//     wildcards. Asserted directly, because a "fix" that escaped them
//     would look like an improvement and would silently change which rows
//     an operator's `audit trail --grep` returns.
//   * ordering is by `id`, not by `recorded_at`. The fixture writes rows
//     that SHARE a timestamp so the two orderings are distinguishable.
//
// Rows are inserted with raw SQL rather than through a verb because
// `audit_log` is written by `planar.policy`'s record path, which this
// bucket deliberately does not depend on — see audit_trail.cppm.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.audit_trail;

namespace {

namespace at = planar::engine::runtime::audit_trail;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_audit_trail_test_{}_{}.db",
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

/// @brief Insert one `audit_log` row.
///
/// `recorded_at` is passed explicitly and SHARED across rows on purpose:
/// the module orders by `id`, and a fixture whose timestamps happen to
/// ascend with the ids cannot tell the two orderings apart.
void add_audit(planar::db::connection& conn, std::string_view kind, std::int64_t id, std::string_view verb,
               std::string_view summary) {
  exec(conn, std::format("insert into audit_log (verb, entity_kind, entity_id, actor, scope, summary, recorded_at) "
                         "values ('{}', '{}', {}, 'tester', 'global', '{}', '2026-01-01T00:00:00.000Z')",
                         verb, kind, id, summary));
}

void add_link(planar::db::connection& conn, std::string_view from_kind, std::int64_t from_id, std::string_view to_kind,
              std::int64_t to_id) {
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('{}', {}, '{}', {}, 'depends-on')",
                         from_kind, from_id, to_kind, to_id));
}

} // namespace

TEST_CASE("for_entity returns only that entity's rows, ordered by id", "[engine][audit_trail]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  add_audit(conn, "task", 1, "create", "first");
  add_audit(conn, "task", 2, "create", "other entity");
  add_audit(conn, "task", 1, "update", "second");
  add_audit(conn, "plan", 1, "create", "other kind, same id");

  auto rows = at::for_entity(conn, "task", 1);
  REQUIRE(rows.has_value());
  // TWO of the four. The other two differ only in `entity_id` and in
  // `entity_kind` respectively, so a predicate that dropped either half of
  // the `(kind, id)` match would return three or four here.
  REQUIRE(rows->size() == 2);
  CHECK((*rows)[0].verb == "create");
  CHECK((*rows)[0].summary == "first");
  CHECK((*rows)[1].verb == "update");
  CHECK((*rows)[1].summary == "second");
  CHECK((*rows)[0].id < (*rows)[1].id);
  // Every row shares one `recorded_at`, so the ascending order above is
  // evidence about `id` specifically.
  CHECK((*rows)[0].recorded_at == (*rows)[1].recorded_at);
  CHECK((*rows)[0].actor == std::optional<std::string>{"tester"});
  CHECK((*rows)[0].scope == std::optional<std::string>{"global"});
}

TEST_CASE("for_entity on an entity with no history returns empty, not an error", "[engine][audit_trail]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto rows = at::for_entity(conn, "task", 4242);
  REQUIRE(rows.has_value());
  CHECK(rows->empty());
}

TEST_CASE("for_entity_with_links follows edges BOTH ways and exactly ONE hop", "[engine][audit_trail][links]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  add_audit(conn, "task", 1, "create", "the subject");
  add_audit(conn, "decision", 7, "create", "outgoing neighbour");
  add_audit(conn, "question", 9, "create", "incoming neighbour");
  add_audit(conn, "plan", 3, "create", "two hops away");
  add_audit(conn, "task", 2, "create", "unrelated entirely");

  // task:1 -> decision:7 (outgoing), question:9 -> task:1 (incoming),
  // decision:7 -> plan:3 (the SECOND hop, which must NOT be followed).
  add_link(conn, "task", 1, "decision", 7);
  add_link(conn, "question", 9, "task", 1);
  add_link(conn, "decision", 7, "plan", 3);

  auto rows = at::for_entity_with_links(conn, "task", 1);
  REQUIRE(rows.has_value());
  // Three: the subject and its two one-hop neighbours. NOT four (the
  // two-hop plan) and NOT five (the unrelated task).
  REQUIRE(rows->size() == 3);

  std::vector<std::string> summaries;
  for (auto const& row : *rows) {
    summaries.push_back(row.summary.value_or(""));
  }
  CHECK(summaries == std::vector<std::string>{"the subject", "outgoing neighbour", "incoming neighbour"});

  // Stated as its own assertion rather than left implicit in the count: a
  // recursive-CTE implementation would pass a size check phrased as `>= 3`.
  for (auto const& row : *rows) {
    CHECK(row.summary != std::optional<std::string>{"two hops away"});
  }
}

TEST_CASE("for_entity_grep matches a substring and treats % as a WILDCARD", "[engine][audit_trail][grep]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  add_audit(conn, "task", 1, "update", "plan_status: draft -> active");
  add_audit(conn, "task", 1, "update", "priority: 100 -> 50");
  add_audit(conn, "task", 1, "update", "plan_status: active -> done");
  // A summary containing a LITERAL percent, so the wildcard case below is
  // distinguishable from a literal-match implementation.
  //
  // The verb is `update` and not something descriptive like `note` because
  // `audit_log.verb` carries a CHECK over six values (migration 00014) and
  // an out-of-vocabulary verb makes the INSERT fail — which is how the
  // first draft of this fixture found out.
  add_audit(conn, "task", 1, "update", "coverage 95% of leaves");

  auto hits = at::for_entity_grep(conn, "task", 1, "plan_status:");
  REQUIRE(hits.has_value());
  REQUIRE(hits->size() == 2);
  CHECK((*hits)[0].summary == std::optional<std::string>{"plan_status: draft -> active"});
  CHECK((*hits)[1].summary == std::optional<std::string>{"plan_status: active -> done"});

  auto none = at::for_entity_grep(conn, "task", 1, "nosuchsentinel");
  REQUIRE(none.has_value());
  CHECK(none->empty());

  // `%` inside the pattern is a SQL wildcard, not a literal. There is no
  // `escape` clause, so `coverage%leaves` matches the percent-containing
  // row by spanning it. Asserted so an "escaping fix" fails here rather
  // than silently narrowing an operator's grep.
  auto wild = at::for_entity_grep(conn, "task", 1, "coverage%leaves");
  REQUIRE(wild.has_value());
  CHECK(wild->size() == 1);

  // And the same pattern read LITERALLY would match nothing, which is what
  // makes the assertion above evidence rather than a coincidence.
  auto literal_probe = at::for_entity_grep(conn, "task", 1, "coverage 95");
  REQUIRE(literal_probe.has_value());
  CHECK(literal_probe->size() == 1);
}

TEST_CASE("for_entity_grep never matches a NULL summary", "[engine][audit_trail][grep]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into audit_log (verb, entity_kind, entity_id, recorded_at) "
             "values ('create', 'task', 1, '2026-01-01T00:00:00.000Z')");

  // `null like '%%'` is NULL, not true — so even the everything-pattern
  // misses it. `for_entity` still sees the row, which is what proves the
  // row exists and the miss is the `like` semantics rather than an empty
  // fixture.
  auto all = at::for_entity(conn, "task", 1);
  REQUIRE(all.has_value());
  CHECK(all->size() == 1);
  CHECK(!(*all)[0].summary.has_value());

  auto grepped = at::for_entity_grep(conn, "task", 1, "");
  REQUIRE(grepped.has_value());
  CHECK(grepped->empty());
}

TEST_CASE("session_timeline returns the header and entries ordered by ordinal", "[engine][audit_trail][session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'todo', 100)");
  exec(conn, "insert into sessions (task_id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  // Inserted OUT of ordinal order so the `order by ordinal` is load-bearing
  // rather than accidentally satisfied by insertion order.
  exec(conn, "insert into session_entries (session_id, ordinal, prefix, body) values (1, 2, 'note', 'second')");
  exec(conn, "insert into session_entries (session_id, ordinal, prefix, body) values (1, 1, 'action', 'first')");

  auto t = at::session_timeline(conn, 1);
  REQUIRE(t.has_value());
  CHECK(t->session_id == 1);
  CHECK(t->vendor == "claude");
  CHECK(t->task_id == std::optional<std::int64_t>{1});
  CHECK(t->started_at == "2026-01-01T00:00:00.000Z");
  CHECK(!t->ended_at.has_value());
  REQUIRE(t->entries.size() == 2);
  CHECK(t->entries[0].ordinal == 1);
  CHECK(t->entries[0].prefix == "action");
  CHECK(t->entries[0].body == "first");
  CHECK(t->entries[1].ordinal == 2);
  CHECK(t->entries[1].body == "second");
}

TEST_CASE("session_timeline reports not_found for an id no session has", "[engine][audit_trail][session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto missing = at::session_timeline(conn, 99);
  REQUIRE(!missing.has_value());
  // not_found, NOT query_failed — the leaf above it maps the two to
  // different exit codes.
  CHECK(missing.error() == at::audit_error::not_found);
}

TEST_CASE("session_timeline carries an unbound, ended session with no task", "[engine][audit_trail][session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  exec(conn, "insert into sessions (vendor, started_at, ended_at) "
             "values ('cli', '2026-01-01T00:00:00.000Z', '2026-01-02T00:00:00.000Z')");

  auto t = at::session_timeline(conn, 1);
  REQUIRE(t.has_value());
  // The two optionals move INDEPENDENTLY, and the leaf's `--json` omits
  // each on its own, so both are pinned in the state opposite to the case
  // above.
  CHECK(!t->task_id.has_value());
  CHECK(t->ended_at == std::optional<std::string>{"2026-01-02T00:00:00.000Z"});
  CHECK(t->entries.empty());
}
