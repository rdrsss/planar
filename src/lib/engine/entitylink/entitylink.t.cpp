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
