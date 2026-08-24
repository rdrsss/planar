// @file plan.t.cpp
// @brief Unit tests for `planar.engine.planning.plan` (plan 996, task
// cpp-planning-verbs). Exercises CRUD, the plan status transition matrix
// (via `update`), the parent-cycle guard, and the plan-304 auto-promotion
// invariant (`recompute_status`) against a real, migrated on-disk SQLite
// database. Include-before-import per db.t.cpp / scope.t.cpp precedent.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.plan;

namespace {

using planar::engine::planning::create_plan;
using planar::engine::planning::list_plans;
using planar::engine::planning::plan;
using planar::engine::planning::plan_create_args;
using planar::engine::planning::plan_error;
using planar::engine::planning::plan_list_filter;
using planar::engine::planning::plan_status;
using planar::engine::planning::plan_update_args;
using planar::engine::planning::recompute_status;
using planar::engine::planning::show_plan;
using planar::engine::planning::update_plan;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_planning_plan_test_{}_{}.db",
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

auto insert_task(planar::db::connection& conn, std::int64_t plan_id, std::string_view status) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, plan_id, title, status) values ('global', ?, 'a task', ?) "
                           "returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, plan_id).has_value());
  REQUIRE(stmt->bind_text(2, status).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

} // namespace

// --- CRUD ----------------------------------------------------------------

TEST_CASE("create + show round-trip a global plan", "[plan][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto p = create_plan(conn, plan_create_args{.title = "My Plan", .summary = "a summary"});
  REQUIRE(p.has_value());
  CHECK(p->title == "My Plan");
  CHECK(p->slug == "my-plan");
  CHECK(p->status == plan_status::draft);
  REQUIRE(p->summary.has_value());
  CHECK(*p->summary == "a summary");

  auto shown = show_plan(conn, p->id);
  REQUIRE(shown.has_value());
  CHECK(shown->id == p->id);
  CHECK(shown->title == "My Plan");
}

TEST_CASE("show: absent id returns not_found", "[plan][crud][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = show_plan(conn, 999999);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == plan_error::not_found);
}

TEST_CASE("create: explicit slug collision returns slug_conflict", "[plan][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p1   = create_plan(conn, plan_create_args{.title = "First", .slug = "dup"});
  REQUIRE(p1.has_value());
  auto p2 = create_plan(conn, plan_create_args{.title = "Second", .slug = "dup"});
  REQUIRE_FALSE(p2.has_value());
  CHECK(p2.error() == plan_error::slug_conflict);
}

TEST_CASE("list: empty database returns an empty list", "[plan][crud][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = list_plans(conn, plan_list_filter{});
  REQUIRE(r.has_value());
  CHECK(r->empty());
}

TEST_CASE("update: no-op patch is a successful read-only show", "[plan][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "Untouched"});
  REQUIRE(p.has_value());
  auto updated = update_plan(conn, p->id, plan_update_args{});
  REQUIRE(updated.has_value());
  CHECK(updated->title == "Untouched");
}

TEST_CASE("update: parent cycle is refused", "[plan][crud]") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto            outer = create_plan(conn, plan_create_args{.title = "Outer"});
  REQUIRE(outer.has_value());
  auto inner = create_plan(conn, plan_create_args{.title = "Inner", .parent_plan_id = outer->id});
  REQUIRE(inner.has_value());

  auto r = update_plan(conn, outer->id, plan_update_args{.parent_plan_id = inner->id});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == plan_error::invalid_parent_cycle);
}

// --- status transition rules (via update) ---------------------------------

TEST_CASE("update: legal plan transition draft -> active succeeds", "[plan][transitions]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T"});
  REQUIRE(p.has_value());
  auto r = update_plan(conn, p->id, plan_update_args{.status = plan_status::active});
  REQUIRE(r.has_value());
  CHECK(r->status == plan_status::active);
}

TEST_CASE("update: illegal plan transition draft -> done is refused with illegal_transition", "[plan][transitions]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T"});
  REQUIRE(p.has_value());
  auto r = update_plan(conn, p->id, plan_update_args{.status = plan_status::done});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == plan_error::illegal_transition);
  // The refusal must not have mutated the row.
  auto reshown = show_plan(conn, p->id);
  REQUIRE(reshown.has_value());
  CHECK(reshown->status == plan_status::draft);
}

TEST_CASE("update: illegal plan transition from a terminal status is refused", "[plan][transitions]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T", .status = plan_status::abandoned});
  REQUIRE(p.has_value());
  auto r = update_plan(conn, p->id, plan_update_args{.status = plan_status::active});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == plan_error::illegal_transition);
}

// --- recompute_status: plan-304 auto-promotion -----------------------------

TEST_CASE("recompute_status: all child-plan tasks done flips a non-anchor plan to done; flipped=true", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = create_plan(conn, plan_create_args{.title = "Anchor"});
  REQUIRE(anchor.has_value());
  auto child = create_plan(conn, plan_create_args{.title = "Child", .status = plan_status::active, .parent_plan_id = anchor->id});
  REQUIRE(child.has_value());
  insert_task(conn, child->id, "done");
  insert_task(conn, child->id, "cancelled");

  auto result = recompute_status(conn, child->id);
  REQUIRE(result.has_value());
  CHECK(result->flipped);
  CHECK(result->status_before == plan_status::active);
  CHECK(result->status_after == plan_status::done);

  auto reshown = show_plan(conn, child->id);
  REQUIRE(reshown.has_value());
  CHECK(reshown->status == plan_status::done);
}

TEST_CASE("recompute_status: anchor plan never auto-promotes past active even when all tasks are terminal", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = create_plan(conn, plan_create_args{.title = "Anchor", .status = plan_status::active});
  REQUIRE(anchor.has_value());
  insert_task(conn, anchor->id, "done");

  auto result = recompute_status(conn, anchor->id);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->flipped);
  CHECK(result->status_after == plan_status::active);
}

TEST_CASE("recompute_status: some tasks still open does NOT flip; flipped=false", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T", .status = plan_status::active});
  REQUIRE(p.has_value());
  insert_task(conn, p->id, "done");
  insert_task(conn, p->id, "todo");

  auto result = recompute_status(conn, p->id);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->flipped);
  CHECK(result->status_after == plan_status::active);
}

TEST_CASE("recompute_status: draft plan with an active (doing) task promotes to active", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T"});
  REQUIRE(p.has_value());
  insert_task(conn, p->id, "doing");

  auto result = recompute_status(conn, p->id);
  REQUIRE(result.has_value());
  CHECK(result->flipped);
  CHECK(result->status_before == plan_status::draft);
  CHECK(result->status_after == plan_status::active);
}

TEST_CASE("recompute_status: idempotent — a second call on an already-done plan is a no-op", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = create_plan(conn, plan_create_args{.title = "Anchor"});
  REQUIRE(anchor.has_value());
  auto child = create_plan(conn, plan_create_args{.title = "Child", .status = plan_status::active, .parent_plan_id = anchor->id});
  REQUIRE(child.has_value());
  insert_task(conn, child->id, "done");

  auto r1 = recompute_status(conn, child->id);
  REQUIRE(r1.has_value());
  CHECK(r1->flipped);

  auto r2 = recompute_status(conn, child->id);
  REQUIRE(r2.has_value());
  CHECK_FALSE(r2->flipped);
  CHECK(r2->status_before == plan_status::done);
  CHECK(r2->status_after == plan_status::done);
}

TEST_CASE("recompute_status: empty plan (no tasks) never flips regardless of status", "[plan][promotion][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T", .status = plan_status::active});
  REQUIRE(p.has_value());

  auto result = recompute_status(conn, p->id);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->flipped);
}

TEST_CASE("recompute_status: paused plan is a strict no-op regardless of aggregate", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = create_plan(conn, plan_create_args{.title = "T", .status = plan_status::paused});
  REQUIRE(p.has_value());
  insert_task(conn, p->id, "done");

  auto result = recompute_status(conn, p->id);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->flipped);
  CHECK(result->status_after == plan_status::paused);
}

TEST_CASE("recompute_status: single-plan only — a parent plan is NOT touched by a child's recompute", "[plan][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            parent = create_plan(conn, plan_create_args{.title = "Parent"});
  REQUIRE(parent.has_value());
  auto child = create_plan(conn, plan_create_args{.title = "Child", .status = plan_status::active, .parent_plan_id = parent->id});
  REQUIRE(child.has_value());
  insert_task(conn, child->id, "done");

  auto result = recompute_status(conn, child->id);
  REQUIRE(result.has_value());
  CHECK(result->flipped);

  auto parent_reshown = show_plan(conn, parent->id);
  REQUIRE(parent_reshown.has_value());
  CHECK(parent_reshown->status == plan_status::draft); // untouched
}

TEST_CASE("recompute_status: absent plan id returns not_found", "[plan][promotion][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = recompute_status(conn, 999999);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == plan_error::not_found);
}

TEST_CASE("render_text emits parent and summary independently, parent first", "[plan][render][6133]") {
  // Two conditional lines with a NON-obvious relative order: `parent:`
  // prints BEFORE `summary:` even though `summary` comes first in the
  // struct. The three-way split below (neither / summary only / both) is
  // what distinguishes the oracle's order from the struct's — a renderer
  // written from the field list passes the "both absent" case and fails
  // here.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto bare = create_plan(conn, plan_create_args{.title = "Bare"});
  REQUIRE(bare.has_value());
  auto const bare_text = planar::engine::planning::render_text(*bare);
  CHECK(bare_text.starts_with("id:       1\n"
                              "title:    Bare\n"
                              "slug:     bare\n"
                              "status:   draft\n"
                              "scope:    global\n"
                              "created:  "));
  CHECK_FALSE(bare_text.contains("parent:"));
  CHECK_FALSE(bare_text.contains("summary:"));

  auto summarised = create_plan(conn, plan_create_args{.title = "Summarised", .summary = "some text"});
  REQUIRE(summarised.has_value());
  CHECK(planar::engine::planning::render_text(*summarised)
            .contains("scope:    global\n"
                      "summary:  some text\n"
                      "created:  "));

  auto both = create_plan(conn, plan_create_args{.title = "Both", .summary = "s", .parent_plan_id = bare->id});
  REQUIRE(both.has_value());
  CHECK(planar::engine::planning::render_text(*both).contains("scope:    global\n"
                                                              "parent:   1\n"
                                                              "summary:  s\n"
                                                              "created:  "));
}

TEST_CASE("render_json nulls the unset optionals and escapes the set ones", "[plan][render][6133]") {
  // `scope_id`, `summary` and `parent_plan_id` must render as the JSON
  // literal `null`, not as `""` or as an omitted key — a consumer indexing
  // by key would break on omission and a `""` would be indistinguishable
  // from a genuinely empty summary.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto bare = create_plan(conn, plan_create_args{.title = "Bare"});
  REQUIRE(bare.has_value());
  auto const bare_json = planar::engine::planning::render_json(*bare);
  CHECK(bare_json.starts_with(R"({"id":1,"scope_kind":"global","scope_id":null,"title":"Bare","slug":"bare",)"
                              R"("summary":null,"status":"draft","parent_plan_id":null,)"));
  // A FRAGMENT: the caller appends the terminator (see the @return).
  CHECK(bare_json.ends_with("}"));
  CHECK_FALSE(bare_json.ends_with("}\n"));

  auto tricky = create_plan(
      conn, plan_create_args{
                .title = R"(Quote"Title)", .summary = R"(a "b" c\d)", .status = plan_status::active, .parent_plan_id = bare->id});
  REQUIRE(tricky.has_value());
  CHECK(planar::engine::planning::render_json(*tricky).contains(
      R"("title":"Quote\"Title","slug":"quote-title","summary":"a \"b\" c\\d","status":"active",)"
      R"("parent_plan_id":1,)"));
}
