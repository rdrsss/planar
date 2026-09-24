// @file promotion.t.cpp
// @brief Unit tests for `planar.engine.promotion` (plan 996, task 6094).
//
// EVERY expectation below was derived by RUNNING the Zig oracle against a
// scratch database, never from `--help` text and never from reading the
// Zig source. The capture session (reproducible verbatim):
//
//   cd /tmp/oracle/repo && git init -q .
//   export PLANAR_DB=/tmp/oracle/p8.db PLANAR_CONFIG_PATH=/tmp/oracle/p.toml
//   Z=./zig/zig-out/bin/planar
//   $Z init
//   $Z assoc create alpha --name Alpha --kind org
//   $Z assoc create beta  --name Beta  --kind org
//   $Z plan create "Pg" --slug pg --scope global --json
//   $Z promote plan:1 --to alpha --json     # -> exit 0, [json-promote]
//   $Z promote plan:1 --to alpha --json     # -> exit 1, [scope-unchanged]
//   $Z promote plan:1 --to beta             # -> exit 0, [text-promote]
//   $Z demote  plan:1 --json                # -> exit 0, [json-demote]
//   $Z demote  plan:1 --json                # -> exit 1, [demote-unchanged]
//   $Z demote  plan:1                       # -> exit 0, [text-demote]
//   $Z promote plan:1 --to repo:repo --json # -> exit 1, [repo-refused]
//   $Z promote plan:1 --to nope --json      # -> exit 1, [slug-not-found]
//   $Z promote plan:999 --to alpha --json   # -> exit 1, [preread-notfound]
//   $Z promote session:1 --to alpha --json  # -> exit 1, [preread-badkind]
//
// The cross-scope-guard probe (hazard 2 in this task's brief), run in a
// separate scratch DB /tmp/oracle/p5.db with the cwd project JOINED to
// association `alpha` (`$Z assoc add alpha`) and the plan created under
// `--scope beta`:
//
//   $Z scope show --json                 # -> resolved_scopes: repo:repo
//   $Z demote  plan:1 --json             # -> exit 0   <-- NO GUARD
//   $Z promote plan:1 --to beta  --json  # -> exit 0   <-- NO GUARD
//   $Z promote plan:1 --to alpha --json  # -> exit 0
//
// i.e. the oracle consults no cross-scope guard on either verb. Task 6075
// is an OPEN operator decision on whether it should; this suite pins the
// OBSERVED behavior and deliberately adds no guard coverage.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.promotion;

namespace {

using planar::engine::promotion::demote;
using planar::engine::promotion::promote;
using planar::engine::promotion::promote_args;
using planar::engine::promotion::promote_error;
using planar::engine::promotion::read_entity_scope;
using planar::engine::promotion::render_demote_text;
using planar::engine::promotion::render_promote_text;
using planar::engine::promotion::render_scope_change_json;
using planar::engine::promotion::scope_info;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_promotion_test_{}_{}.db",
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

auto scalar_int(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto scalar_text(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

auto insert_assoc(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn, std::format("insert into associations (slug, name, kind) values ('{}', '{}', 'org')", slug, slug));
  return scalar_int(conn, std::format("select id from associations where slug = '{}'", slug));
}

auto insert_global_plan(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn,
       std::format("insert into plans (scope_kind, title, slug, status) values ('global', '{}', '{}', 'draft')", slug, slug));
  return scalar_int(conn, std::format("select id from plans where slug = '{}'", slug));
}

} // namespace

TEST_CASE("promote moves a global plan into an association", "[promotion]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "alpha");
  const auto      plan_id  = insert_global_plan(conn, "pg");

  auto ok = promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"});
  REQUIRE(ok.has_value());

  auto after = read_entity_scope(conn, "plan", plan_id);
  REQUIRE(after.has_value());
  CHECK(after->scope_kind == "association");
  REQUIRE(after->scope_id.has_value());
  CHECK(*after->scope_id == assoc_id);
}

TEST_CASE("promote to the association the entity already sits in is refused", "[promotion]") {
  // Oracle: `$Z promote plan:1 --to alpha --json` twice -> second exits 1
  // with `error: plan:1 is already at scope 'alpha'`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  const auto plan_id = insert_global_plan(conn, "pg");

  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());

  auto again = promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"});
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == promote_error::scope_unchanged);
}

TEST_CASE("promote between two different associations is allowed", "[promotion]") {
  // Oracle: after `--to alpha`, `--to beta` exits 0 and reports
  // `(was: association:1)`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  const auto beta_id = insert_assoc(conn, "beta");
  const auto plan_id = insert_global_plan(conn, "pg");

  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "beta"}).has_value());

  auto after = read_entity_scope(conn, "plan", plan_id);
  REQUIRE(after.has_value());
  REQUIRE(after->scope_id.has_value());
  CHECK(*after->scope_id == beta_id);
}

TEST_CASE("promote --to global demotes, matching the Zig delegation", "[promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  const auto plan_id = insert_global_plan(conn, "pg");
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());

  auto ok = promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "global"});
  REQUIRE(ok.has_value());

  auto after = read_entity_scope(conn, "plan", plan_id);
  REQUIRE(after.has_value());
  CHECK(after->scope_kind == "global");
  CHECK_FALSE(after->scope_id.has_value());
}

TEST_CASE("a repo: target is refused before any slug lookup happens", "[promotion]") {
  // Oracle: `$Z promote plan:1 --to repo:repo --json` -> exit 1,
  // `error: repo: scopes are not supported`. The scratch DB in that probe
  // HAD a project slugged `repo`, and it was still refused -- the prefix
  // check precedes resolution. This test uses a repo slug that does NOT
  // exist, so a port that resolved first would report slug_not_found here
  // and fail.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      plan_id = insert_global_plan(conn, "pg");

  auto res = promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "repo:no-such-project"});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == promote_error::unsupported_scope);
}

TEST_CASE("an unknown association slug is slug_not_found", "[promotion]") {
  // Oracle: `error: no association with slug 'nope'`, exit 1.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      plan_id = insert_global_plan(conn, "pg");

  auto res = promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "nope"});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == promote_error::slug_not_found);
}

TEST_CASE("slug resolution is checked before the entity is read", "[promotion]") {
  // PROVENANCE NOTE, stated honestly: unlike every other case in this file,
  // this ordering is NOT observable through the oracle's CLI. The handler
  // pre-reads the entity scope before calling the engine at all, so
  // `$Z promote plan:999 --to nope --json` reports
  // `reading entity scope: NotFound` and the engine is never entered --
  // both orderings look identical from outside the binary. The ordering is
  // still a real property of the ported engine function, so it is pinned
  // here rather than left to drift; it is flagged as source-derived rather
  // than run-derived so a reviewer does not mistake it for an oracle
  // capture.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = promote(conn, promote_args{.kind = "plan", .id = 4242, .to_scope = "nope"});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == promote_error::slug_not_found);
}

TEST_CASE("a missing entity id is not_found", "[promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");

  auto res = promote(conn, promote_args{.kind = "plan", .id = 4242, .to_scope = "alpha"});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == promote_error::not_found);
}

TEST_CASE("a non-promotable entity kind is invalid_scope on both verbs", "[promotion]") {
  // Oracle: `$Z promote session:1 --to alpha --json` -> exit 1,
  // `error: reading entity scope: InvalidScope` (the handler's PRE-read
  // is what reports it; see read_entity_scope's doc comment).
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");

  auto p = promote(conn, promote_args{.kind = "session", .id = 1, .to_scope = "alpha"});
  REQUIRE_FALSE(p.has_value());
  CHECK(p.error() == promote_error::invalid_scope);

  auto d = demote(conn, "session", 1);
  REQUIRE_FALSE(d.has_value());
  CHECK(d.error() == promote_error::invalid_scope);

  auto r = read_entity_scope(conn, "session", 1);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == promote_error::invalid_scope);
}

TEST_CASE("the kind-to-table map covers exactly the six promotable kinds", "[promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  for (const std::string_view kind : {"plan", "task", "question", "test_scenario", "artifact", "decision"}) {
    auto r = read_entity_scope(conn, kind, 1);
    REQUIRE_FALSE(r.has_value());
    // An empty table yields not_found, NOT invalid_scope -- which proves
    // the kind resolved to a real table and the SELECT actually ran.
    CHECK(r.error() == promote_error::not_found);
  }
  for (const std::string_view kind : {"plan_step", "session", "repo", "annotation", "bogus", ""}) {
    auto r = read_entity_scope(conn, kind, 1);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == promote_error::invalid_scope);
  }
}

TEST_CASE("promote works for tasks, not just plans", "[promotion]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "taskorg");
  exec(conn, "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'todo', 100)");
  const auto task_id = scalar_int(conn, "select id from tasks where title = 'T'");

  REQUIRE(promote(conn, promote_args{.kind = "task", .id = task_id, .to_scope = "taskorg"}).has_value());
  auto after = read_entity_scope(conn, "task", task_id);
  REQUIRE(after.has_value());
  REQUIRE(after->scope_id.has_value());
  CHECK(*after->scope_id == assoc_id);
}

TEST_CASE("demote clears scope_id to SQL NULL, not to zero", "[promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  const auto plan_id = insert_global_plan(conn, "pg");
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());

  REQUIRE(demote(conn, "plan", plan_id).has_value());
  CHECK(scalar_int(conn, std::format("select count(*) from plans where id = {} and scope_id is null", plan_id)) == 1);
}

TEST_CASE("demoting an already-global entity is refused", "[promotion]") {
  // Oracle: `error: plan:1 is already at global scope`, exit 1.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      plan_id = insert_global_plan(conn, "pg");

  auto res = demote(conn, "plan", plan_id);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == promote_error::scope_unchanged);
}

TEST_CASE("demoting a missing entity is not_found", "[promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = demote(conn, "plan", 4242);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == promote_error::not_found);
}

TEST_CASE("a successful scope change bumps updated_at", "[promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  const auto plan_id = insert_global_plan(conn, "pg");
  exec(conn, std::format("update plans set updated_at = '2000-01-01T00:00:00.000Z' where id = {}", plan_id));

  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());
  CHECK(scalar_text(conn, std::format("select updated_at from plans where id = {}", plan_id)) != "2000-01-01T00:00:00.000Z");
}

TEST_CASE("a refused promote leaves the row untouched", "[promotion]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      plan_id = insert_global_plan(conn, "pg");
  exec(conn, std::format("update plans set updated_at = '2000-01-01T00:00:00.000Z' where id = {}", plan_id));

  REQUIRE_FALSE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "nope"}).has_value());
  CHECK(scalar_text(conn, std::format("select updated_at from plans where id = {}", plan_id)) == "2000-01-01T00:00:00.000Z");
  CHECK(scalar_text(conn, std::format("select scope_kind from plans where id = {}", plan_id)) == "global");
}

TEST_CASE("promote and demote consult NO cross-scope guard", "[promotion][hazard-6075]") {
  // HAZARD 2, answered by experiment (see this file's header for the exact
  // oracle session). With the cwd project joined to `alpha` and the plan
  // living in `beta`, the oracle promoted and demoted it with exit 0. This
  // engine module therefore takes no write-scope argument at all: there is
  // nothing for a guard to compare against. Task 6075 is the OPEN operator
  // decision on whether that is correct; this test pins today's behavior so
  // any future guard wiring is a deliberate, visible change here.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  insert_assoc(conn, "beta");
  const auto plan_id = insert_global_plan(conn, "pg");

  // Park the entity in `beta`, then mutate it while nothing anywhere names
  // `beta` as the operator's scope.
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "beta"}).has_value());
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());
  REQUIRE(demote(conn, "plan", plan_id).has_value());
}

TEST_CASE("the --json envelope is byte-identical to the oracle's", "[promotion][parity]") {
  // Captured verbatim from `$Z promote plan:1 --to alpha --json` and
  // `$Z demote plan:1 --json` (see this file's header), INCLUDING the trailing
  // newline: handlers/promote.zig:117 writes `}\n` from the same call that
  // writes the body, and these renderers return the leaf's complete stdout
  // payload. See engine/promotion/CMakeLists.txt for the contract.
  CHECK(render_scope_change_json("plan", 1, scope_info{.scope_kind = "association", .scope_id = 1},
                                 scope_info{.scope_kind = "global", .scope_id = std::nullopt}) ==
        R"({"ok":true,"kind":"plan","id":1,"scope_kind":"association","scope_id":1,)"
        R"("previous_scope_kind":"global","previous_scope_id":null})"
        "\n");

  CHECK(render_scope_change_json("plan", 1, scope_info{.scope_kind = "global", .scope_id = std::nullopt},
                                 scope_info{.scope_kind = "association", .scope_id = 2}) ==
        R"({"ok":true,"kind":"plan","id":1,"scope_kind":"global","scope_id":null,)"
        R"("previous_scope_kind":"association","previous_scope_id":2})"
        "\n");
}

TEST_CASE("the text success lines are byte-identical to the oracle's", "[promotion][parity]") {
  // Captured verbatim from `$Z promote plan:1 --to beta` and
  // `$Z demote plan:1`. Note the DOUBLE space before `(was:` in both --
  // a single space here would silently diverge from the oracle.
  CHECK(render_promote_text("plan", 1, "beta", scope_info{.scope_kind = "association", .scope_id = 1}) ==
        "plan:1 promoted to association beta  (was: association:1)\n");
  CHECK(render_demote_text("plan", 1, scope_info{.scope_kind = "association", .scope_id = 1}) ==
        "plan:1 demoted to global  (was: association:1)\n");
  // A global previous scope renders as the bare kind, with no `:id` tail.
  CHECK(render_promote_text("task", 42, "acme", scope_info{.scope_kind = "global", .scope_id = std::nullopt}) ==
        "task:42 promoted to association acme  (was: global)\n");
}

TEST_CASE("the text output echoes the --to flag verbatim, not the resolved slug", "[promotion][parity]") {
  // The oracle prints `args.to`, so an `assoc:`-prefixed value survives
  // into the output unnormalised.
  CHECK(render_promote_text("plan", 1, "assoc:beta", scope_info{.scope_kind = "global", .scope_id = std::nullopt}) ==
        "plan:1 promoted to association assoc:beta  (was: global)\n");
}

// =========================================================================
// Audit (task 6184)
// =========================================================================
//
// Oracle-captured shape (running `$Z promote plan:1 --to project:planar
// --json` then `$Z demote plan:1 --from project:planar --json` against a
// scratch DB, `select * from audit_log`):
//
//   status_change|plan|1|||promote plan:1 from global to association:1
//   status_change|plan|1|||demote plan:1 from association:1 to global
//
// `actor`/`scope` are NULL on both rows, matching every other wired site.

namespace {

struct audit_row {
  std::string                verb;
  std::string                entity_kind;
  std::int64_t               entity_id;
  bool                       actor_is_null;
  bool                       scope_is_null;
  std::optional<std::string> summary;
};

auto last_audit_row(planar::db::connection& conn) -> audit_row {
  auto stmt = conn.prepare("select verb, entity_kind, entity_id, actor, scope, summary "
                           "from audit_log order by id desc limit 1");
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  audit_row row{.verb          = stmt->column_text(0),
                .entity_kind   = stmt->column_text(1),
                .entity_id     = stmt->column_int64(2),
                .actor_is_null = stmt->is_null(3),
                .scope_is_null = stmt->is_null(4),
                .summary       = std::nullopt};
  if (!stmt->is_null(5)) {
    row.summary = stmt->column_text(5);
  }
  return row;
}

} // namespace

TEST_CASE("promote writes an oracle-shaped best-effort audit row", "[promotion][audit]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "alpha");
  const auto      plan_id  = insert_global_plan(conn, "pg");

  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());

  auto row = last_audit_row(conn);
  CHECK(row.verb == "status_change");
  CHECK(row.entity_kind == "plan");
  CHECK(row.entity_id == plan_id);
  CHECK(row.actor_is_null);
  CHECK(row.scope_is_null);
  REQUIRE(row.summary.has_value());
  CHECK(*row.summary == std::format("promote plan:{} from global to association:{}", plan_id, assoc_id));
}

TEST_CASE("demote writes an oracle-shaped best-effort audit row", "[promotion][audit]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "alpha");
  const auto      plan_id  = insert_global_plan(conn, "pg");
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());

  REQUIRE(demote(conn, "plan", plan_id).has_value());

  auto row = last_audit_row(conn);
  CHECK(row.verb == "status_change");
  CHECK(row.entity_kind == "plan");
  CHECK(row.entity_id == plan_id);
  CHECK(row.actor_is_null);
  CHECK(row.scope_is_null);
  REQUIRE(row.summary.has_value());
  CHECK(*row.summary == std::format("demote plan:{} from association:{} to global", plan_id, assoc_id));
}

TEST_CASE("promote's audit write is best-effort: a broken audit_log does not fail the verb", "[promotion][audit]") {
  // This is the asymmetry task 6184 calls out explicitly: promotion.zig
  // `catch {}`s the audit write so a failed row cannot roll back an
  // already-committed scope change. Every other wired bucket instead
  // propagates the failure (see association.cpp's `record_audit`). Drop
  // the table out from under the connection to force `audit::record` to
  // fail, then assert the scope mutation still lands.
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_assoc(conn, "alpha");
  const auto      plan_id  = insert_global_plan(conn, "pg");
  exec(conn, "drop table audit_log");

  auto ok = promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"});
  REQUIRE(ok.has_value());

  auto after = read_entity_scope(conn, "plan", plan_id);
  REQUIRE(after.has_value());
  CHECK(after->scope_kind == "association");
  REQUIRE(after->scope_id.has_value());
  CHECK(*after->scope_id == assoc_id);
}

TEST_CASE("demote's audit write is best-effort: a broken audit_log does not fail the verb", "[promotion][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_assoc(conn, "alpha");
  const auto plan_id = insert_global_plan(conn, "pg");
  REQUIRE(promote(conn, promote_args{.kind = "plan", .id = plan_id, .to_scope = "alpha"}).has_value());
  exec(conn, "drop table audit_log");

  auto ok = demote(conn, "plan", plan_id);
  REQUIRE(ok.has_value());

  auto after = read_entity_scope(conn, "plan", plan_id);
  REQUIRE(after.has_value());
  CHECK(after->scope_kind == "global");
}
