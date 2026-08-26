// @file entitylink.t.cpp
// @brief Unit tests for `planar.engine.entitylink` (plan 996, task
// cpp-entity-links). Exercises the `EntityKind`/`Relationship` text
// round-trips, `parse_ref`, the `entity_links` CRUD surface (add/remove/
// show/list), the `task_touch_paths` repo-edge-vs-path-row distinction,
// and — the load-bearing acceptance item — that `add` succeeds on a
// cross-scope edge with no guard consulted (link verbs are UNGUARDED BY
// DESIGN, docs/concepts.md § cross-scope-guard).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.entitylink;

namespace {

using planar::engine::entitylink::add;
using planar::engine::entitylink::add_touch_path;
using planar::engine::entitylink::entity_kind;
using planar::engine::entitylink::entity_kind_from_text;
using planar::engine::entitylink::entity_kind_to_text;
using planar::engine::entitylink::entity_link_add_args;
using planar::engine::entitylink::entity_link_error;
using planar::engine::entitylink::entity_link_list_filter;
using planar::engine::entitylink::list;
using planar::engine::entitylink::missing_endpoint;
using planar::engine::entitylink::missing_endpoint_of;
using planar::engine::entitylink::parse_ref;
using planar::engine::entitylink::relationship;
using planar::engine::entitylink::relationship_from_text;
using planar::engine::entitylink::relationship_to_text;
using planar::engine::entitylink::remove;
using planar::engine::entitylink::remove_touch_path;
using planar::engine::entitylink::show;
using planar::engine::entitylink::touched_paths;
using planar::engine::entitylink::touched_repo_ids;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_entitylink_test_{}_{}.db",
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

// plans.slug is NOT NULL; derive a unique-enough slug from the title.
auto insert_plan(planar::db::connection& conn, std::string_view title) -> std::int64_t {
  auto stmt = conn.prepare("insert into plans (scope_kind, title, slug, status) values ('global', ?, ?, 'draft') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_text(2, title).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

// A minimal global-scope task.
auto insert_task_global(planar::db::connection& conn, std::string_view title) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, scope_id, title, status, priority) "
                           "values ('global', null, ?, 'todo', 100) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

// A repo-scoped task, scope_id pointing at a projects row.
auto insert_task_repo_scoped(planar::db::connection& conn, std::string_view title, std::int64_t repo_id) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, scope_id, title, status, priority) "
                           "values ('repo', ?, ?, 'todo', 100) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, repo_id).has_value());
  REQUIRE(stmt->bind_text(2, title).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_project(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into projects (slug, name) values (?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

} // namespace

// --- EntityKind / Relationship round-trip -----------------------------------

TEST_CASE("entity_kind_from_text / to_text round-trip for every kind", "[entitylink][kind]") {
  for (auto k :
       {"plan", "plan_step", "task", "question", "test_scenario", "artifact", "decision", "session", "repo", "annotation"}) {
    auto ek = entity_kind_from_text(k);
    REQUIRE(ek.has_value());
    CHECK(entity_kind_to_text(*ek) == std::string_view(k));
  }
}

TEST_CASE("entity_kind_from_text returns unset for unknown kind", "[entitylink][kind]") {
  CHECK_FALSE(entity_kind_from_text("bogus").has_value());
  CHECK_FALSE(entity_kind_from_text("").has_value());
}

TEST_CASE("relationship_from_text / to_text round-trip for every relationship", "[entitylink][relationship]") {
  for (auto r : {"derives-from", "depends-on", "addresses", "verifies", "cites", "supersedes", "touches"}) {
    auto rel = relationship_from_text(r);
    REQUIRE(rel.has_value());
    CHECK(relationship_to_text(*rel) == std::string_view(r));
  }
}

TEST_CASE("relationship_from_text rejects 'blocks' (pre-migration-00033 spelling)", "[entitylink][relationship]") {
  CHECK_FALSE(relationship_from_text("blocks").has_value());
  CHECK_FALSE(relationship_from_text("nope").has_value());
}

// --- parse_ref ---------------------------------------------------------------

TEST_CASE("parse_ref: 'plan:42' yields an id ref", "[entitylink][parse_ref]") {
  auto r = parse_ref("plan:42");
  REQUIRE(r.has_value());
  auto* id = std::get_if<planar::engine::entitylink::parsed_ref::id_ref>(&r->value);
  REQUIRE(id != nullptr);
  CHECK(id->kind == entity_kind::plan);
  CHECK(id->id == 42);
}

TEST_CASE("parse_ref: 'plan:some-slug' yields a slug ref", "[entitylink][parse_ref]") {
  auto r = parse_ref("plan:some-slug");
  REQUIRE(r.has_value());
  auto* slug = std::get_if<planar::engine::entitylink::parsed_ref::slug_ref>(&r->value);
  REQUIRE(slug != nullptr);
  CHECK(slug->kind == entity_kind::plan);
  CHECK(slug->slug == "some-slug");
}

TEST_CASE("parse_ref: malformed inputs return invalid_ref", "[entitylink][parse_ref]") {
  for (auto s : {"", "plan", ":42", "plan:", "bogus:42", "plan:0", "plan:-1"}) {
    auto r = parse_ref(s);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == entity_link_error::invalid_ref);
  }
}

// zig's `std.fmt.parseInt` fails on a PARTIAL numeric parse (trailing
// garbage), not just a non-numeric string — and entitylink.zig's `parseRef`
// treats ANY integer-parse failure (partial or total) as "not an integer,
// treat as a slug", not as `Error.InvalidRef`. So a ref like "42abc" is a
// valid (if unusual) slug, matching the oracle exactly.
TEST_CASE("parse_ref: a partially-numeric tail is a slug, not invalid_ref", "[entitylink][parse_ref]") {
  auto r = parse_ref("plan:42abc");
  REQUIRE(r.has_value());
  auto* slug = std::get_if<planar::engine::entitylink::parsed_ref::slug_ref>(&r->value);
  REQUIRE(slug != nullptr);
  CHECK(slug->kind == entity_kind::plan);
  CHECK(slug->slug == "42abc");
}

// --- add ----------------------------------------------------------------------

TEST_CASE("add: happy path between two existing plans", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "Plan A");
  auto            p2   = insert_plan(conn, "Plan B");

  auto link = add(conn, entity_link_add_args{
                            .from_kind     = entity_kind::plan,
                            .from_id       = p1,
                            .to_kind       = entity_kind::plan,
                            .to_id         = p2,
                            .relationship_ = relationship::derives_from,
                        });
  REQUIRE(link.has_value());
  CHECK(link->from_kind == entity_kind::plan);
  CHECK(link->from_id == p1);
  CHECK(link->to_kind == entity_kind::plan);
  CHECK(link->to_id == p2);
  CHECK(link->relationship_ == relationship::derives_from);
  CHECK_FALSE(link->created_at.empty());
}

TEST_CASE("add: duplicate (from, to, relationship) returns link_exists", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "Plan A");
  auto            p2   = insert_plan(conn, "Plan B");

  auto first = add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                              .from_id       = p1,
                                              .to_kind       = entity_kind::plan,
                                              .to_id         = p2,
                                              .relationship_ = relationship::cites});
  REQUIRE(first.has_value());

  auto second = add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                               .from_id       = p1,
                                               .to_kind       = entity_kind::plan,
                                               .to_id         = p2,
                                               .relationship_ = relationship::cites});
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error() == entity_link_error::link_exists);
}

TEST_CASE("add: a different relationship between the same pair is NOT a duplicate", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "Plan A");
  auto            p2   = insert_plan(conn, "Plan B");

  auto first = add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                              .from_id       = p1,
                                              .to_kind       = entity_kind::plan,
                                              .to_id         = p2,
                                              .relationship_ = relationship::cites});
  REQUIRE(first.has_value());
  auto second = add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                               .from_id       = p1,
                                               .to_kind       = entity_kind::plan,
                                               .to_id         = p2,
                                               .relationship_ = relationship::depends_on});
  REQUIRE(second.has_value());
}

TEST_CASE("add: non-null scope returns unsupported_scope", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto r = add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                          .from_id       = 1,
                                          .to_kind       = entity_kind::plan,
                                          .to_id         = 2,
                                          .relationship_ = relationship::depends_on,
                                          .scope         = "some-assoc"});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::unsupported_scope);
}

TEST_CASE("add: skip_scope_check=true with no scope still succeeds (accepted, not consulted)", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "Plan A");
  auto            p2   = insert_plan(conn, "Plan B");

  auto link = add(conn, entity_link_add_args{.from_kind        = entity_kind::plan,
                                             .from_id          = p1,
                                             .to_kind          = entity_kind::plan,
                                             .to_id            = p2,
                                             .relationship_    = relationship::touches,
                                             .skip_scope_check = true});
  REQUIRE(link.has_value());
}

TEST_CASE("add: missing 'from' endpoint returns endpoint_not_found", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p2   = insert_plan(conn, "Plan B");

  auto args = entity_link_add_args{.from_kind     = entity_kind::plan,
                                   .from_id       = 99999,
                                   .to_kind       = entity_kind::plan,
                                   .to_id         = p2,
                                   .relationship_ = relationship::cites};
  auto miss = missing_endpoint_of(conn, args);
  REQUIRE(miss.has_value());
  CHECK(*miss == missing_endpoint::from);

  auto r = add(conn, args);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::endpoint_not_found);
}

TEST_CASE("add: missing 'to' endpoint returns endpoint_not_found", "[entitylink][add]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "Plan A");

  auto args = entity_link_add_args{.from_kind     = entity_kind::plan,
                                   .from_id       = p1,
                                   .to_kind       = entity_kind::plan,
                                   .to_id         = 99999,
                                   .relationship_ = relationship::cites};
  auto miss = missing_endpoint_of(conn, args);
  REQUIRE(miss.has_value());
  CHECK(*miss == missing_endpoint::to);

  auto r = add(conn, args);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::endpoint_not_found);
}

// --- remove ---------------------------------------------------------------

TEST_CASE("remove: happy path removes the row", "[entitylink][remove]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "P1");
  auto            p2   = insert_plan(conn, "P2");
  auto            link = add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                                        .from_id       = p1,
                                                        .to_kind       = entity_kind::plan,
                                                        .to_id         = p2,
                                                        .relationship_ = relationship::depends_on});
  REQUIRE(link.has_value());

  auto r = remove(conn, link->id);
  REQUIRE(r.has_value());

  auto shown = show(conn, link->id);
  REQUIRE_FALSE(shown.has_value());
  CHECK(shown.error() == entity_link_error::not_found);
}

TEST_CASE("remove: missing id returns not_found", "[entitylink][remove][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = remove(conn, 99999);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::not_found);
}

// --- show -------------------------------------------------------------------

TEST_CASE("show: missing id returns not_found", "[entitylink][show][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = show(conn, 99999);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::not_found);
}

// --- list ---------------------------------------------------------------------

TEST_CASE("list: filter by from_kind + from_id returns matches", "[entitylink][list]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "P1");
  auto            p2   = insert_plan(conn, "P2");
  auto            p3   = insert_plan(conn, "P3");

  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p1,
                                         .to_kind       = entity_kind::plan,
                                         .to_id         = p2,
                                         .relationship_ = relationship::cites})
              .has_value());
  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p1,
                                         .to_kind       = entity_kind::plan,
                                         .to_id         = p3,
                                         .relationship_ = relationship::depends_on})
              .has_value());
  // Different from_id — should not appear.
  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p2,
                                         .to_kind       = entity_kind::plan,
                                         .to_id         = p3,
                                         .relationship_ = relationship::addresses})
              .has_value());

  auto results = list(conn, entity_link_list_filter{.from_kind = entity_kind::plan, .from_id = p1});
  REQUIRE(results.has_value());
  CHECK(results->size() == 2);
}

TEST_CASE("list: filter by relationship returns matches", "[entitylink][list]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "P1");
  auto            p2   = insert_plan(conn, "P2");
  auto            p3   = insert_plan(conn, "P3");

  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p1,
                                         .to_kind       = entity_kind::plan,
                                         .to_id         = p2,
                                         .relationship_ = relationship::cites})
              .has_value());
  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p2,
                                         .to_kind       = entity_kind::plan,
                                         .to_id         = p3,
                                         .relationship_ = relationship::depends_on})
              .has_value());

  auto results = list(conn, entity_link_list_filter{.relationship_ = relationship::cites});
  REQUIRE(results.has_value());
  REQUIRE(results->size() == 1);
  CHECK((*results)[0].relationship_ == relationship::cites);
}

TEST_CASE("list: filter by from-kind + to-kind pair distinguishes plan-plan from plan-task", "[entitylink][list]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = insert_plan(conn, "P1");
  auto            p2   = insert_plan(conn, "P2");
  auto            t1   = insert_task_global(conn, "T1");

  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p1,
                                         .to_kind       = entity_kind::plan,
                                         .to_id         = p2,
                                         .relationship_ = relationship::cites})
              .has_value());
  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::plan,
                                         .from_id       = p1,
                                         .to_kind       = entity_kind::task,
                                         .to_id         = t1,
                                         .relationship_ = relationship::touches})
              .has_value());

  auto plan_to_plan = list(conn, entity_link_list_filter{.from_kind = entity_kind::plan, .to_kind = entity_kind::plan});
  REQUIRE(plan_to_plan.has_value());
  CHECK(plan_to_plan->size() == 1);

  auto plan_to_task = list(conn, entity_link_list_filter{.from_kind = entity_kind::plan, .to_kind = entity_kind::task});
  REQUIRE(plan_to_task.has_value());
  CHECK(plan_to_task->size() == 1);
}

TEST_CASE("list: empty database returns an empty vector, not an error", "[entitylink][list][empty]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            results = list(conn, entity_link_list_filter{});
  REQUIRE(results.has_value());
  CHECK(results->empty());
}

TEST_CASE("list: non-null scope returns unsupported_scope", "[entitylink][list]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = list(conn, entity_link_list_filter{.scope = "some-assoc"});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::unsupported_scope);
}

// --- unguarded cross-scope edge (load-bearing acceptance item) --------------
//
// Link verbs are documented UNGUARDED (docs/concepts.md §
// cross-scope-guard). This module does not even depend on `engine_identity`
// (see CMakeLists.txt) so there is no guard to call in the first place —
// this test proves the OBSERVABLE consequence: `add` succeeds linking two
// tasks that live under deliberately DIFFERENT scopes (one global, one
// repo-scoped), with no scope resolution or guard check anywhere on the
// path.

TEST_CASE("add: succeeds across two different scopes with no guard consulted", "[entitylink][unguarded]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto repo_id     = insert_project(conn, "repo-a");
  auto global_task = insert_task_global(conn, "Global task");
  auto repo_task   = insert_task_repo_scoped(conn, "Repo-scoped task", repo_id);

  // Sanity: the two tasks really do carry different scope_kind values —
  // this is the "may legitimately cross scopes" shape the polyrepo
  // touches/derives-from workflow relies on.
  {
    auto stmt = conn.prepare("select scope_kind from tasks where id = ?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, global_task).has_value());
    auto step = stmt->step();
    REQUIRE(step.has_value());
    REQUIRE(*step == planar::db::step_result::row);
    CHECK(stmt->column_text(0) == "global");
  }
  {
    auto stmt = conn.prepare("select scope_kind from tasks where id = ?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, repo_task).has_value());
    auto step = stmt->step();
    REQUIRE(step.has_value());
    REQUIRE(*step == planar::db::step_result::row);
    CHECK(stmt->column_text(0) == "repo");
  }

  auto link = add(conn, entity_link_add_args{
                            .from_kind     = entity_kind::task,
                            .from_id       = global_task,
                            .to_kind       = entity_kind::task,
                            .to_id         = repo_task,
                            .relationship_ = relationship::depends_on,
                        });
  REQUIRE(link.has_value());
  CHECK(link->from_id == global_task);
  CHECK(link->to_id == repo_task);
}

// --- task_touch_paths: repo-edge vs path-row distinction --------------------

TEST_CASE("add_touch_path + touched_paths round-trip path-level touches", "[entitylink][touches]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            repo_id = insert_project(conn, "repo-a");
  auto            task_id = insert_task_global(conn, "T1");

  REQUIRE(add_touch_path(conn, task_id, repo_id, "src/a.cpp").has_value());
  REQUIRE(add_touch_path(conn, task_id, repo_id, "migrations/00099_x.sql").has_value());

  auto paths = touched_paths(conn, task_id);
  REQUIRE(paths.has_value());
  REQUIRE(paths->size() == 2);
  CHECK((*paths)[0].path == "migrations/00099_x.sql");
  CHECK((*paths)[1].path == "src/a.cpp");
}

TEST_CASE("add_touch_path is idempotent on the unique(task,repo,path) constraint", "[entitylink][touches]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            repo_id = insert_project(conn, "repo-a");
  auto            task_id = insert_task_global(conn, "T1");

  REQUIRE(add_touch_path(conn, task_id, repo_id, "src/a.cpp").has_value());
  REQUIRE(add_touch_path(conn, task_id, repo_id, "src/a.cpp").has_value()); // duplicate is a no-op

  auto paths = touched_paths(conn, task_id);
  REQUIRE(paths.has_value());
  CHECK(paths->size() == 1);
}

TEST_CASE("touched_paths returns an empty vector for a task with no declared paths", "[entitylink][touches][empty]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            task_id = insert_task_global(conn, "T1");

  auto paths = touched_paths(conn, task_id);
  REQUIRE(paths.has_value());
  CHECK(paths->empty());
}

TEST_CASE("remove_touch_path: missing row returns not_found", "[entitylink][touches][empty]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            repo_id = insert_project(conn, "repo-a");
  auto            task_id = insert_task_global(conn, "T1");

  auto r = remove_touch_path(conn, task_id, repo_id, "no/such/path");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == entity_link_error::not_found);
}

// This is THE distinction called out in this task's brief: a path-level row
// and the coarse repo-level `entity_links` edge are independent — removing
// one must NOT withdraw the other.

TEST_CASE("removing the coarse repo-touches edge does NOT withdraw a path-level declaration", "[entitylink][touches]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            repo_id = insert_project(conn, "repo-a");
  auto            task_id = insert_task_global(conn, "T1");

  // Write both the coarse repo edge and a path-level declaration, exactly
  // as `task touches add --path` does at the cmd/ layer (composition cut
  // from this module — see CMakeLists.txt — but the invariant it relies on
  // lives here).
  auto edge = add(conn, entity_link_add_args{.from_kind     = entity_kind::task,
                                             .from_id       = task_id,
                                             .to_kind       = entity_kind::repo,
                                             .to_id         = repo_id,
                                             .relationship_ = relationship::touches});
  REQUIRE(edge.has_value());
  REQUIRE(add_touch_path(conn, task_id, repo_id, "src/a.cpp").has_value());

  // Remove ONLY the coarse repo edge.
  REQUIRE(remove(conn, edge->id).has_value());

  // The path-level declaration must still be there.
  auto paths = touched_paths(conn, task_id);
  REQUIRE(paths.has_value());
  REQUIRE(paths->size() == 1);
  CHECK((*paths)[0].path == "src/a.cpp");

  // And the coarse edge is really gone (touched_repo_ids reads it).
  auto repo_ids = touched_repo_ids(conn, task_id);
  REQUIRE(repo_ids.has_value());
  CHECK(repo_ids->empty());
}

TEST_CASE("removing a path-level declaration does NOT withdraw the coarse repo-touches edge", "[entitylink][touches]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            repo_id = insert_project(conn, "repo-a");
  auto            task_id = insert_task_global(conn, "T1");

  REQUIRE(add(conn, entity_link_add_args{.from_kind     = entity_kind::task,
                                         .from_id       = task_id,
                                         .to_kind       = entity_kind::repo,
                                         .to_id         = repo_id,
                                         .relationship_ = relationship::touches})
              .has_value());
  REQUIRE(add_touch_path(conn, task_id, repo_id, "src/a.cpp").has_value());

  REQUIRE(remove_touch_path(conn, task_id, repo_id, "src/a.cpp").has_value());

  auto paths = touched_paths(conn, task_id);
  REQUIRE(paths.has_value());
  CHECK(paths->empty());

  auto repo_ids = touched_repo_ids(conn, task_id);
  REQUIRE(repo_ids.has_value());
  REQUIRE(repo_ids->size() == 1);
  CHECK((*repo_ids)[0] == repo_id);
}

TEST_CASE("touched_repo_ids returns an empty vector for a task with no touches edges", "[entitylink][touches][empty]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            task_id = insert_task_global(conn, "T1");

  auto repo_ids = touched_repo_ids(conn, task_id);
  REQUIRE(repo_ids.has_value());
  CHECK(repo_ids->empty());
}

// --- audit trail (task 6193) ------------------------------------------------
//
// The write half (`policy.audit.record` in `add`/`remove`) and the read
// half (`trail`) landed together at task 6193. They had been deferred as a
// PAIR on the reasoning that a write with no reader cannot be proven, and
// these cases are that proof: every one asserts the audit_log ROWS, not
// just the function's return.

namespace {

using planar::engine::entitylink::audit_row;
using planar::engine::entitylink::directed_link;
using planar::engine::entitylink::entity_link;
using planar::engine::entitylink::merge_directed;
using planar::engine::entitylink::render_entity_link_json;
using planar::engine::entitylink::render_entity_link_text;
using planar::engine::entitylink::render_link_exists_error;
using planar::engine::entitylink::render_link_list_json;
using planar::engine::entitylink::render_link_list_text;
using planar::engine::entitylink::render_links_add_json;
using planar::engine::entitylink::render_links_add_text;
using planar::engine::entitylink::render_links_remove_json;
using planar::engine::entitylink::render_links_remove_text;
using planar::engine::entitylink::render_trail_json;
using planar::engine::entitylink::render_trail_text;
using planar::engine::entitylink::trail;

// `verb|entity_kind|entity_id|actor|scope|summary`, SQL NULL as `<NULL>`.
auto audit_dump(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select verb, entity_kind, entity_id, actor, scope, summary from audit_log order by id");
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ';';
    }
    for (int col = 0; col < 6; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

// Append one audit_log row directly, for the multi-row and decoy fixtures
// no CLI path can produce.
void insert_audit(planar::db::connection& conn, std::string_view verb, std::string_view kind, std::int64_t id) {
  auto stmt = conn.prepare("insert into audit_log (verb, entity_kind, entity_id) values (?, ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, verb).has_value());
  REQUIRE(stmt->bind_text(2, kind).has_value());
  REQUIRE(stmt->bind_int64(3, id).has_value());
  REQUIRE(stmt->step().has_value());
}

} // namespace

TEST_CASE("add records link|entity_link and remove records unlink, with all three optionals NULL", "[entitylink][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      a    = insert_plan(conn, "a");
  auto const      b    = insert_plan(conn, "b");

  auto created = add(conn, entity_link_add_args{
                               .from_kind     = entity_kind::plan,
                               .from_id       = a,
                               .to_kind       = entity_kind::plan,
                               .to_id         = b,
                               .relationship_ = relationship::depends_on,
                           });
  REQUIRE(created.has_value());
  // NULL, not "". All three render as JSON `null` and `actor` renders as
  // the text literal `(none)`; a port that bound "" would be visible to
  // an operator and invisible to a verb/entity_id assertion.
  CHECK(audit_dump(conn) == std::format("link|entity_link|{}|<NULL>|<NULL>|<NULL>", created->id));

  REQUIRE(remove(conn, created->id).has_value());
  CHECK(audit_dump(conn) == std::format("link|entity_link|{}|<NULL>|<NULL>|<NULL>;"
                                        "unlink|entity_link|{}|<NULL>|<NULL>|<NULL>",
                                        created->id, created->id));
}

TEST_CASE("a REFUSED add writes no audit row", "[entitylink][audit]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      a    = insert_plan(conn, "a");

  // Missing endpoint, and duplicate. Neither may leave an audit row: the
  // record call is placed AFTER the insert precisely so a refusal cannot.
  auto missing = add(conn, entity_link_add_args{
                               .from_kind     = entity_kind::plan,
                               .from_id       = a,
                               .to_kind       = entity_kind::plan,
                               .to_id         = 9999,
                               .relationship_ = relationship::cites,
                           });
  REQUIRE_FALSE(missing.has_value());
  CHECK(audit_dump(conn).empty());

  auto const b  = insert_plan(conn, "b");
  auto const ok = add(conn, entity_link_add_args{
                                .from_kind     = entity_kind::plan,
                                .from_id       = a,
                                .to_kind       = entity_kind::plan,
                                .to_id         = b,
                                .relationship_ = relationship::cites,
                            });
  REQUIRE(ok.has_value());
  auto const before = audit_dump(conn);

  auto dup = add(conn, entity_link_add_args{
                           .from_kind     = entity_kind::plan,
                           .from_id       = a,
                           .to_kind       = entity_kind::plan,
                           .to_id         = b,
                           .relationship_ = relationship::cites,
                       });
  REQUIRE_FALSE(dup.has_value());
  CHECK(dup.error() == entity_link_error::link_exists);
  CHECK(audit_dump(conn) == before);

  // ...and a refused REMOVE likewise.
  auto gone = remove(conn, 9999);
  REQUIRE_FALSE(gone.has_value());
  CHECK(gone.error() == entity_link_error::not_found);
  CHECK(audit_dump(conn) == before);
}

TEST_CASE("trail returns MULTIPLE rows ordered by id, excluding both decoy shapes", "[entitylink][trail]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      a    = insert_plan(conn, "a");
  auto const      b    = insert_plan(conn, "b");
  auto const      c    = insert_plan(conn, "c");

  auto one = add(conn, entity_link_add_args{
                           .from_kind     = entity_kind::plan,
                           .from_id       = a,
                           .to_kind       = entity_kind::plan,
                           .to_id         = b,
                           .relationship_ = relationship::cites,
                       });
  REQUIRE(one.has_value());
  auto two = add(conn, entity_link_add_args{
                           .from_kind     = entity_kind::plan,
                           .from_id       = a,
                           .to_kind       = entity_kind::plan,
                           .to_id         = c,
                           .relationship_ = relationship::cites,
                       });
  REQUIRE(two.has_value());

  // A SECOND row for link `one` — unreachable through the CLI, because the
  // only other writer is `remove`, which makes the link unfindable. So the
  // ordering of a multi-row trail can only be pinned here.
  insert_audit(conn, "update", "entity_link", one->id);
  // DECOY A: same entity_id, DIFFERENT kind. Dropping `entity_kind` from
  // the predicate pulls this in.
  insert_audit(conn, "create", "plan", one->id);
  // DECOY B: same kind, DIFFERENT id — link `two`'s row already is one.

  auto rows = trail(conn, one->id);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  CHECK((*rows)[0].verb == "link");
  CHECK((*rows)[1].verb == "update");
  CHECK((*rows)[0].id < (*rows)[1].id); // ordered by id, not by insertion luck
  for (auto const& row : *rows) {
    CHECK(row.entity_kind == "entity_link");
    CHECK(row.entity_id == one->id);
    // NULL survives the read as unset, not as "".
    CHECK_FALSE(row.actor.has_value());
    CHECK_FALSE(row.scope.has_value());
    CHECK_FALSE(row.summary.has_value());
    CHECK_FALSE(row.recorded_at.empty());
  }

  // A DIFFERENT link gives a DIFFERENT answer — an inert entity_id
  // predicate returns the same set for both.
  auto other = trail(conn, two->id);
  REQUIRE(other.has_value());
  REQUIRE(other->size() == 1);
  CHECK((*other)[0].entity_id == two->id);

  // THE SURVIVORS. Both decoys are still in audit_log, so the exclusions
  // above are real rather than an artifact of absent rows.
  CHECK(audit_dump(conn).contains("create|plan|"));
  CHECK(audit_dump(conn).contains(std::format("link|entity_link|{}|", two->id)));
}

TEST_CASE("trail refuses a missing link but succeeds EMPTY on a link with no rows", "[entitylink][trail]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      a    = insert_plan(conn, "a");
  auto const      b    = insert_plan(conn, "b");

  auto created = add(conn, entity_link_add_args{
                               .from_kind     = entity_kind::plan,
                               .from_id       = a,
                               .to_kind       = entity_kind::plan,
                               .to_id         = b,
                               .relationship_ = relationship::cites,
                           });
  REQUIRE(created.has_value());

  auto missing = trail(conn, 9999);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == entity_link_error::not_found);

  {
    auto stmt = conn.prepare("delete from audit_log");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
  }
  // An EXISTING link with no rows is a SUCCESS holding an empty vector —
  // a different answer from `not_found`, and the CLI gives them different
  // exit codes.
  auto empty = trail(conn, created->id);
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
}

// --- renderers --------------------------------------------------------------

TEST_CASE("render_link_list_text forces a + sign and OVERFLOWS rather than truncating", "[entitylink][render]") {
  entity_link                      small{.id            = 2,
                                         .from_kind     = entity_kind::task,
                                         .from_id       = 1,
                                         .to_kind       = entity_kind::task,
                                         .to_id         = 2,
                                         .relationship_ = relationship::depends_on,
                                         .created_at    = "2026-01-01T00:00:00.000Z"};
  entity_link                      huge{.id            = 1234567,
                                        .from_kind     = entity_kind::task,
                                        .from_id       = 1,
                                        .to_kind       = entity_kind::plan,
                                        .to_id         = 9,
                                        .relationship_ = relationship::cites,
                                        .created_at    = "2026-01-01T00:00:00.000Z"};
  std::vector<directed_link> const rows{{.link = small, .outbound = true}, {.link = huge, .outbound = false}};

  // Oracle-captured, including the `+`: a five-digit id still fits width
  // 6, a seven-digit one pushes the rest of the line right instead of
  // being cut. Both were verified by seeding the ids and running.
  CHECK(render_link_list_text(rows, entity_kind::task, 1) == "id     direction  relationship     peer\n"
                                                             "+2     from       depends-on       task:2\n"
                                                             "+1234567 to         cites            task:1\n");
}

TEST_CASE("the two EMPTY renderings disagree, and both JSON ones are zero bytes", "[entitylink][render]") {
  std::vector<directed_link> const no_links;
  std::vector<audit_row> const     no_trail;

  CHECK(render_link_list_text(no_links, entity_kind::task, 999) == "no links for task:999\n");
  CHECK(render_trail_text(no_trail, 7) == "no audit trail for entity_link:7\n");
  // NOT `[]`, NOT `"\n"`. This is why these renderers own their
  // terminators instead of following the fragment contract.
  CHECK(render_link_list_json(no_links).empty());
  CHECK(render_trail_json(no_trail).empty());
}

TEST_CASE("render_trail_text prints (none) for a NULL actor and the value for a set one", "[entitylink][render]") {
  std::vector<audit_row> const rows{
      {.id          = 13,
       .verb        = "link",
       .entity_kind = "entity_link",
       .entity_id   = 2,
       .actor       = std::nullopt,
       .scope       = std::nullopt,
       .summary     = std::nullopt,
       .recorded_at = "2026-08-26T15:17:48.633Z"},
      // The discrimination: a SET actor must render verbatim, so `(none)`
      // is proven to be the NULL branch and not a constant.
      {.id          = 14,
       .verb        = "unlink",
       .entity_kind = "entity_link",
       .entity_id   = 2,
       .actor       = "agent-7",
       .scope       = "global",
       .summary     = "s",
       .recorded_at = "2026-08-26T15:17:49.000Z"},
  };
  CHECK(render_trail_text(rows, 2) == "id     verb         actor        recorded_at\n"
                                      "+13    link         (none)       2026-08-26T15:17:48.633Z\n"
                                      "+14    unlink       agent-7      2026-08-26T15:17:49.000Z\n");

  // ...and the JSON half keeps NULL and a value apart too.
  auto const json = render_trail_json(rows);
  CHECK(json.contains(R"("actor":null,"scope":null,"summary":null)"));
  CHECK(json.contains(R"("actor":"agent-7","scope":"global","summary":"s")"));
  CHECK(std::ranges::count(json, '\n') == 2);
}

TEST_CASE("the three single-object JSON envelopes are distinct", "[entitylink][render]") {
  entity_link const link{.id            = 3,
                         .from_kind     = entity_kind::plan,
                         .from_id       = 1,
                         .to_kind       = entity_kind::task,
                         .to_id         = 2,
                         .relationship_ = relationship::addresses,
                         .created_at    = "2026-01-01T00:00:00.000Z"};

  // `links add` — `ok`, from-side columns, NO created_at.
  CHECK(render_links_add_json(link, "addresses") ==
        R"({"ok":true,"id":3,"from_kind":"plan","from_id":1,"to_kind":"task","to_id":2,"relationship":"addresses"})"
        "\n");
  // `<entity> link` — `ok`, a PER-VERB subject key, no from-side columns.
  CHECK(render_entity_link_json("plan_id", 1, link, "addresses") ==
        R"({"ok":true,"id":3,"plan_id":1,"to_kind":"task","to_id":2,"relationship":"addresses"})"
        "\n");
  CHECK(render_entity_link_json("question_id", 5, link, "addresses") ==
        R"({"ok":true,"id":3,"question_id":5,"to_kind":"task","to_id":2,"relationship":"addresses"})"
        "\n");
  // `links remove` — two fields only.
  CHECK(render_links_remove_json(3) == "{\"ok\":true,\"id\":3}\n");

  // ...and `links list`'s, which is the only one carrying created_at and
  // the only one WITHOUT `ok`.
  std::vector<directed_link> const rows{{.link = link, .outbound = true}};
  CHECK(render_link_list_json(rows) ==
        R"({"id":3,"from_kind":"plan","from_id":1,"to_kind":"task","to_id":2,"relationship":"addresses",)"
        R"("created_at":"2026-01-01T00:00:00.000Z"})"
        "\n");
}

TEST_CASE("the two success sentences differ in wording AND in where the double spaces fall", "[entitylink][render]") {
  entity_link const link{.id            = 10,
                         .from_kind     = entity_kind::task,
                         .from_id       = 1,
                         .to_kind       = entity_kind::task,
                         .to_id         = 2,
                         .relationship_ = relationship::cites,
                         .created_at    = "2026-01-01T00:00:00.000Z"};

  CHECK(render_links_add_text(link, "cites") == "created entity_link: task:1 --[cites]--> task:2  (link id: 10)\n");
  CHECK(render_entity_link_text(entity_kind::plan, 1, link, "cites") == "linked plan:1 -> task:2  [cites]  (link id: 10)\n");
  CHECK(render_links_remove_text(10) == "entity link 10 removed\n");
}

TEST_CASE("render_link_exists_error takes the arrow as a REQUIRED parameter", "[entitylink][render]") {
  // The oracle is INCONSISTENT here — `plan link` writes U+2192, its two
  // siblings and `links add` write `->` — so the arrow cannot be a
  // constant and must not have a default that a call site inherits
  // silently.
  CHECK(render_link_exists_error(entity_kind::plan, 1, entity_kind::task, 1, "depends-on", true) ==
        "link plan:1 → task:1 [depends-on] already exists");
  CHECK(render_link_exists_error(entity_kind::task, 1, entity_kind::plan, 2, "derives-from", false) ==
        "link task:1 -> plan:2 [derives-from] already exists");
  // No `error: ` prefix and no trailing newline — this one is a message
  // BODY the reporting site composes around.
  CHECK_FALSE(render_link_exists_error(entity_kind::plan, 1, entity_kind::task, 1, "cites", true).starts_with("error:"));
  CHECK_FALSE(render_link_exists_error(entity_kind::plan, 1, entity_kind::task, 1, "cites", true).ends_with("\n"));
}

TEST_CASE("merge_directed lists a self-link once and preserves from-then-to order", "[entitylink][render]") {
  auto const make = [](std::int64_t id, std::int64_t from_id, std::int64_t to_id) {
    return entity_link{.id            = id,
                       .from_kind     = entity_kind::task,
                       .from_id       = from_id,
                       .to_kind       = entity_kind::task,
                       .to_id         = to_id,
                       .relationship_ = relationship::cites,
                       .created_at    = "2026-01-01T00:00:00.000Z"};
  };
  // Link 5 is a SELF-link and so appears in BOTH half-queries; it must be
  // listed ONCE, as `from`.
  std::vector<entity_link> const from_side{make(3, 1, 2), make(5, 1, 1)};
  std::vector<entity_link> const to_side{make(5, 1, 1), make(9, 4, 1)};

  auto const merged = merge_directed(from_side, to_side);
  REQUIRE(merged.size() == 3);
  CHECK(merged[0].link.id == 3);
  CHECK(merged[0].outbound);
  CHECK(merged[1].link.id == 5);
  CHECK(merged[1].outbound); // `from` wins for the self-link
  CHECK(merged[2].link.id == 9);
  CHECK_FALSE(merged[2].outbound);

  // The non-degenerate control: with no overlap nothing is dropped.
  auto const disjoint = merge_directed(std::vector<entity_link>{make(1, 1, 2)}, std::vector<entity_link>{make(2, 3, 1)});
  REQUIRE(disjoint.size() == 2);
}
