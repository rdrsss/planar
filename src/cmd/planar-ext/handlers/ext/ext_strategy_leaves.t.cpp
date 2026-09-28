// @file ext_strategy_leaves.t.cpp
// @brief Unit tests for `planar.cmd.planar_ext.handlers.ext_strategy` (plan
// 996, task 6421). Lifted from the halted `salvage/task-6409-ext-rehome`
// lane and re-homed to this binary's module name and namespace. Drives
// `select_strategy` directly against a scratch database -- it is a pure
// composition of two already-tested lower-layer functions, so this pins the
// COMPOSITION rather than duplicating either one's own coverage.
//
// ## What is pinned here vs. one layer down
//
// `distinct_repo_count_in_feature`'s own recursion/dedup/query-failure
// coverage lives in `parent_issue.t.cpp` and is NOT re-derived here. What
// IS pinned here is the COMPOSITION: that `select_strategy` reaches the
// right bucket at each ADR-0006 boundary (0/1/2 repos), that the dedup
// behaviour survives one layer up (a second task touching an
// already-counted repo does not change the selected strategy), that
// `jira` never even issues the count query, and -- the case this task's
// brief calls out by name -- that a count-query failure propagates as
// `query_failed` rather than silently selecting the zero-repo strategy.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.extsync;
import planar.cmd.planar_ext.handlers.ext_strategy;

#include "../../../../engine/external/scratch_db.hpp"

namespace {

namespace handlers = planar::cmd::ext::handlers;

using planar::engine::external::testing::err;
using planar::engine::external::testing::open_migrated;
using planar::engine::external::testing::scratch_db_path;

auto insert_plan(planar::db::connection& conn, std::string_view title, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into plans (scope_kind, title, slug) values ('global', ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_task_under_plan(planar::db::connection& conn, std::string_view title, std::int64_t plan_id) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, title, plan_id) values ('global', ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_int64(2, plan_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_project(planar::db::connection& conn, std::string_view slug, std::string_view git_remote) -> std::int64_t {
  auto stmt = conn.prepare("insert into projects (slug, name, git_remote) values (?, ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->bind_text(3, git_remote).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_touches_edge(planar::db::connection& conn, std::int64_t task_id, std::int64_t project_id) -> void {
  auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values ('task', ?, 'repo', ?, 'touches')");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, task_id).has_value());
  REQUIRE(stmt->bind_int64(2, project_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
}

} // namespace

TEST_CASE("select_strategy: fixture sanity -- the seeded rows are actually present", "[cmd][planar-ext][ext_strategy]") {
  // Every case below is a byte/enum comparison against a query result; a
  // fixture that silently seeded nothing would satisfy every one of them
  // by counting zero rows correctly. Pin the fixture's own shape first.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      anchor_id = insert_plan(conn, "anchor", "a");
  auto const      project   = insert_project(conn, "acme/api", "https://github.com/acme/api.git");
  auto const      t1        = insert_task_under_plan(conn, "t1", anchor_id);
  insert_touches_edge(conn, t1, project);

  auto stmt = conn.prepare("select count(*) from entity_links where relationship = 'touches'");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 1);
}

TEST_CASE("select_strategy: jira is always jira-epic and never touches the database", "[cmd][planar-ext][ext_strategy]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // No plan with this id exists at all. If `select_strategy` issued ANY
  // query keyed on it for the jira path, either the query would return
  // nothing meaningful (harmless) or -- if it followed the github-issues
  // shape -- would report `query_failed`/zero. It must do neither: the
  // oracle returns for "jira" before ever building the count query.
  auto s = handlers::select_strategy(conn, /*anchor_plan_id=*/987654321, "jira");
  REQUIRE_FALSE(err(s).has_value());
  CHECK(s->kind == "jira-epic");
  CHECK(s->plan_anchor_kind == "epic");
  CHECK(s->plan_child_kind == "story");
  CHECK(s->task_kind == "sub-task");
}

TEST_CASE("select_strategy: github-issues buckets by distinct touched repo count at every ADR-0006 boundary",
          "[cmd][planar-ext][ext_strategy]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      anchor_id = insert_plan(conn, "anchor", "a");

  // 0 repos -> github-zero-repo. The boundary `strategy_for_repo_count`
  // actually discriminates at, not an interior value.
  {
    insert_task_under_plan(conn, "touches nothing", anchor_id);
    auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
    REQUIRE_FALSE(err(s).has_value());
    CHECK(s->kind == "github-zero-repo");
  }

  auto const project1 = insert_project(conn, "acme/api", "https://github.com/acme/api.git");
  auto const t1       = insert_task_under_plan(conn, "touches project1", anchor_id);
  insert_touches_edge(conn, t1, project1);

  // 1 repo -> github-parent-issue.
  {
    auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
    REQUIRE_FALSE(err(s).has_value());
    CHECK(s->kind == "github-parent-issue");
  }

  // DEDUP: a second task touching the SAME repo must not inflate the
  // count past 1, and must not flip the selected strategy. This is the
  // composed-boundary version of `distinct_repo_count_in_feature`'s own
  // dedup assertion -- pinned again here because a regression at THIS
  // layer (e.g. a future rewrite that stops delegating and re-counts by
  // hand) would not be caught by that lower-layer test at all.
  auto const t2 = insert_task_under_plan(conn, "touches project1 again", anchor_id);
  insert_touches_edge(conn, t2, project1);
  {
    auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
    REQUIRE_FALSE(err(s).has_value());
    CHECK(s->kind == "github-parent-issue");
  }

  // 2 repos -> github-projects-v2, the other boundary this bucket switch
  // discriminates at.
  auto const project2 = insert_project(conn, "acme/web", "https://github.com/acme/web.git");
  auto const t3       = insert_task_under_plan(conn, "touches project2", anchor_id);
  insert_touches_edge(conn, t3, project2);
  {
    auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
    REQUIRE_FALSE(err(s).has_value());
    CHECK(s->kind == "github-projects-v2");
  }
}

TEST_CASE("select_strategy: github-issues with a repo-scoped task (not just a touches edge) is also parent-issue",
          "[cmd][planar-ext][ext_strategy]") {
  // The oracle's task_repos CTE has TWO arms: `scope_kind='repo'` tasks and
  // `-touches-> repo` entity_links edges. The boundary-coverage case above
  // only exercises the second arm; this pins the first.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      anchor_id = insert_plan(conn, "anchor", "a");
  auto const      project   = insert_project(conn, "acme/api", "https://github.com/acme/api.git");

  auto stmt = conn.prepare("insert into tasks (scope_kind, scope_id, title, plan_id) values ('repo', ?, 't', ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, project).has_value());
  REQUIRE(stmt->bind_int64(2, anchor_id).has_value());
  REQUIRE(stmt->step().has_value());

  auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
  REQUIRE_FALSE(err(s).has_value());
  CHECK(s->kind == "github-parent-issue");
}

TEST_CASE("select_strategy: an unsupported system kind refuses without touching the database",
          "[cmd][planar-ext][ext_strategy]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto s = handlers::select_strategy(conn, /*anchor_plan_id=*/1, "gitlab-issues");
  REQUIRE(err(s).has_value());
  CHECK(*err(s) == handlers::strategy_select_error::unsupported_system_kind);
}

TEST_CASE("select_strategy: a two-repo feature still selects github-projects-v2 by NAME, "
          "even though this binary cannot execute that strategy (decision 1001)",
          "[cmd][planar-ext][ext_strategy]") {
  // `select_strategy` reports the oracle-accurate ADR-0006 bucket
  // regardless of what this binary can DO with it -- the CLI bridge
  // (propagate.cpp) is the layer that refuses a >=2-repo feature, and it
  // needs the real bucket name to build that refusal message. This case
  // pins that `select_strategy` itself does not pre-emptively soften a
  // >=2-repo result into `github-parent-issue` (which would silently
  // mis-attribute a multi-repo feature to the single-repo strategy).
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      anchor_id = insert_plan(conn, "anchor", "a");

  auto const project1 = insert_project(conn, "acme/api", "https://github.com/acme/api.git");
  auto const t1       = insert_task_under_plan(conn, "touches project1", anchor_id);
  insert_touches_edge(conn, t1, project1);
  auto const project2 = insert_project(conn, "acme/web", "https://github.com/acme/web.git");
  auto const t2       = insert_task_under_plan(conn, "touches project2", anchor_id);
  insert_touches_edge(conn, t2, project2);

  auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
  REQUIRE_FALSE(err(s).has_value());
  CHECK(s->kind == "github-projects-v2");
}

TEST_CASE("select_strategy: a distinct-repo-count query failure propagates as query_failed, "
          "NEVER as the zero-repo strategy",
          "[cmd][planar-ext][ext_strategy]") {
  // Reached the same way parent_issue.t.cpp's sibling case reaches it:
  // rename `tasks` out from under the query so `conn.prepare` itself
  // fails. This is the case this task's brief names explicitly -- the
  // gap task 6189 closed at the lower layer must not reopen one layer up
  // by mapping the propagated failure back onto `github-zero-repo`.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      anchor_id = insert_plan(conn, "anchor", "a");

  REQUIRE(conn.execute("alter table tasks rename to tasks_real").has_value());

  auto s = handlers::select_strategy(conn, anchor_id, "github-issues");
  REQUIRE(err(s).has_value());
  CHECK(*err(s) == handlers::strategy_select_error::query_failed);
}
