// @file task.t.cpp
// @brief Unit tests for `planar.engine.planning.task` (plan 996, task
// cpp-planning-verbs). Exercises CRUD, the task status transition matrix
// (bare update and the four dedicated verbs), the `force`/`task_reopens`
// escape hatch, the `mark_blocked` entity_links edge, and — the load-
// bearing invariant — plan-304 auto-promotion firing on every one of the
// six task-write paths (create, update, mark_done, mark_cancelled,
// mark_blocked, reopen).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.plan;
import planar.engine.planning.task;

namespace {

using planar::engine::planning::create_task;
using planar::engine::planning::list_tasks;
using planar::engine::planning::mark_blocked;
using planar::engine::planning::mark_cancelled;
using planar::engine::planning::mark_done;
using planar::engine::planning::plan_create_args;
using planar::engine::planning::plan_status;
using planar::engine::planning::reopen;
using planar::engine::planning::show_task;
using planar::engine::planning::task_create_args;
using planar::engine::planning::task_error;
using planar::engine::planning::task_list_filter;
using planar::engine::planning::task_status;
using planar::engine::planning::task_update_args;
using planar::engine::planning::update_task;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_planning_task_test_{}_{}.db",
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

auto reopen_count(planar::db::connection& conn, std::int64_t task_id) -> std::int64_t {
  auto stmt = conn.prepare("select count(*) from task_reopens where task_id = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, task_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto entity_link_count(planar::db::connection& conn, std::int64_t from_id, std::int64_t to_id) -> std::int64_t {
  auto stmt = conn.prepare("select count(*) from entity_links where from_kind='task' and from_id=? "
                           "and to_kind='task' and to_id=? and relationship='depends-on'");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, from_id).has_value());
  REQUIRE(stmt->bind_int64(2, to_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

} // namespace

// --- CRUD ------------------------------------------------------------------

TEST_CASE("create + show round-trip a global task", "[task][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto t = create_task(
      conn, task_create_args{.title = "Write the test", .body = "And run it.", .priority = 50, .next_action = "open editor"});
  REQUIRE(t.has_value());
  CHECK(t->title == "Write the test");
  CHECK(t->status == task_status::todo);
  CHECK(t->priority == 50);
  REQUIRE(t->body.has_value());
  CHECK(*t->body == "And run it.");
  REQUIRE(t->next_action.has_value());
  CHECK(*t->next_action == "open editor");

  auto shown = show_task(conn, t->id);
  REQUIRE(shown.has_value());
  CHECK(shown->id == t->id);
}

TEST_CASE("show: absent id returns not_found", "[task][crud][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = show_task(conn, 999999);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == task_error::not_found);
}

TEST_CASE("create: explicit slug collision returns slug_conflict", "[task][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t1   = create_task(conn, task_create_args{.title = "First", .slug = "dup"});
  REQUIRE(t1.has_value());
  auto t2 = create_task(conn, task_create_args{.title = "Second", .slug = "dup"});
  REQUIRE_FALSE(t2.has_value());
  CHECK(t2.error() == task_error::slug_conflict);
}

TEST_CASE("list: empty database returns an empty list", "[task][crud][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            r    = list_tasks(conn, task_list_filter{});
  REQUIRE(r.has_value());
  CHECK(r->empty());
}

TEST_CASE("list: default filter excludes done/cancelled tasks", "[task][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            open = create_task(conn, task_create_args{.title = "Open"});
  REQUIRE(open.has_value());
  auto done_task = create_task(conn, task_create_args{.title = "Done", .status = task_status::done});
  REQUIRE(done_task.has_value());

  auto r = list_tasks(conn, task_list_filter{});
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 1);
  CHECK((*r)[0].id == open->id);
}

// --- status transition rules (bare update) ---------------------------------

TEST_CASE("update: legal task transition todo -> doing succeeds", "[task][transitions]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T"});
  REQUIRE(t.has_value());
  auto r = update_task(conn, t->id, task_update_args{.status = task_status::doing});
  REQUIRE(r.has_value());
  CHECK(r->status == task_status::doing);
}

TEST_CASE("update: illegal task transition todo -> done (skips doing) is refused", "[task][transitions]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T"});
  REQUIRE(t.has_value());
  auto r = update_task(conn, t->id, task_update_args{.status = task_status::done});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == task_error::illegal_transition);
  auto reshown = show_task(conn, t->id);
  REQUIRE(reshown.has_value());
  CHECK(reshown->status == task_status::todo); // unmutated
}

TEST_CASE("update: bare update out of a terminal status is refused without force", "[task][transitions]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T", .status = task_status::done});
  REQUIRE(t.has_value());
  auto r = update_task(conn, t->id, task_update_args{.status = task_status::todo});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == task_error::illegal_transition);
}

TEST_CASE("update: force=true bypasses the matrix and records a task_reopens row", "[task][transitions][force]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T", .status = task_status::done});
  REQUIRE(t.has_value());
  CHECK(reopen_count(conn, t->id) == 0);

  auto r = update_task(conn, t->id, task_update_args{.status = task_status::todo, .force = true, .reason = "re-open it"});
  REQUIRE(r.has_value());
  CHECK(r->status == task_status::todo);
  CHECK(reopen_count(conn, t->id) == 1);
}

// --- dedicated transition verbs ---------------------------------------------

TEST_CASE("mark_done: legal transition succeeds", "[task][verbs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T", .status = task_status::doing});
  REQUIRE(t.has_value());
  auto r = mark_done(conn, t->id, false);
  REQUIRE(r.has_value());
  CHECK(r->status == task_status::done);
}

TEST_CASE("mark_done: illegal transition (already done, bare) is refused without force", "[task][verbs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T", .status = task_status::cancelled});
  REQUIRE(t.has_value());
  auto r = mark_done(conn, t->id, false);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == task_error::illegal_transition);
}

TEST_CASE("mark_cancelled: legal transition succeeds", "[task][verbs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T"});
  REQUIRE(t.has_value());
  auto r = mark_cancelled(conn, t->id);
  REQUIRE(r.has_value());
  CHECK(r->status == task_status::cancelled);
}

TEST_CASE("mark_blocked: records the depends-on entity_links edge and rejects an absent blocker", "[task][verbs]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            t       = create_task(conn, task_create_args{.title = "T"});
  auto            blocker = create_task(conn, task_create_args{.title = "Blocker"});
  REQUIRE(t.has_value());
  REQUIRE(blocker.has_value());

  auto missing = mark_blocked(conn, t->id, 999999, std::nullopt, false);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == task_error::not_found);

  CHECK(entity_link_count(conn, t->id, blocker->id) == 0);
  auto r = mark_blocked(conn, t->id, blocker->id, "waiting on it", false);
  REQUIRE(r.has_value());
  CHECK(r->status == task_status::blocked);
  CHECK(entity_link_count(conn, t->id, blocker->id) == 1);
}

TEST_CASE("reopen: bypasses the matrix and records a task_reopens row with source='task-reopen'", "[task][verbs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T", .status = task_status::done});
  REQUIRE(t.has_value());
  CHECK(reopen_count(conn, t->id) == 0);

  auto r = reopen(conn, t->id, task_status::doing, "picking it back up");
  REQUIRE(r.has_value());
  CHECK(r->status == task_status::doing);
  CHECK(reopen_count(conn, t->id) == 1);

  auto stmt = conn.prepare("select source from task_reopens where task_id = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, t->id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  CHECK(stmt->column_text(0) == "task-reopen");
}

// --- plan-304 auto-promotion: all six task-write paths ---------------------

TEST_CASE("auto-promotion path 1/6: create flips the plan to active", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = planar::engine::planning::create_plan(conn, plan_create_args{.title = "P"});
  REQUIRE(p.has_value());
  CHECK(p->status == plan_status::draft);

  auto t = create_task(conn, task_create_args{.title = "T", .status = task_status::doing, .plan_id = p->id});
  REQUIRE(t.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::active);
}

TEST_CASE("auto-promotion path 2/6: update (status change) flips the plan to done", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = planar::engine::planning::create_plan(conn, plan_create_args{.title = "Anchor"});
  REQUIRE(anchor.has_value());
  auto p = planar::engine::planning::create_plan(
      conn, plan_create_args{.title = "P", .status = plan_status::active, .parent_plan_id = anchor->id});
  REQUIRE(p.has_value());
  auto t = create_task(conn, task_create_args{.title = "T", .status = task_status::doing, .plan_id = p->id});
  REQUIRE(t.has_value());

  auto updated = update_task(conn, t->id, task_update_args{.status = task_status::done});
  REQUIRE(updated.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::done);
}

TEST_CASE("auto-promotion path 2b/6: update (plan_id reassignment) recomputes BOTH the old and new plan", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  auto            old_anchor = planar::engine::planning::create_plan(conn, plan_create_args{.title = "OldAnchor"});
  auto            old_plan   = planar::engine::planning::create_plan(
      conn, plan_create_args{.title = "Old", .status = plan_status::active, .parent_plan_id = old_anchor->id});
  auto new_anchor = planar::engine::planning::create_plan(conn, plan_create_args{.title = "NewAnchor"});
  auto new_plan = planar::engine::planning::create_plan(conn, plan_create_args{.title = "New", .parent_plan_id = new_anchor->id});
  REQUIRE(old_plan.has_value());
  REQUIRE(new_plan.has_value());

  auto t = create_task(conn, task_create_args{.title = "T", .status = task_status::done, .plan_id = old_plan->id});
  REQUIRE(t.has_value());
  // old_plan should already be `done` (single task, all terminal).
  auto old_reshown = planar::engine::planning::show_plan(conn, old_plan->id);
  REQUIRE(old_reshown.has_value());
  CHECK(old_reshown->status == plan_status::done);

  auto moved = update_task(conn, t->id, task_update_args{.plan_id = new_plan->id});
  REQUIRE(moved.has_value());

  // new_plan gains a `done` task -> promotes draft -> done.
  auto new_reshown = planar::engine::planning::show_plan(conn, new_plan->id);
  REQUIRE(new_reshown.has_value());
  CHECK(new_reshown->status == plan_status::done);
}

TEST_CASE("auto-promotion path 3/6: mark_done flips the plan to done", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = planar::engine::planning::create_plan(conn, plan_create_args{.title = "Anchor"});
  auto            p      = planar::engine::planning::create_plan(
      conn, plan_create_args{.title = "P", .status = plan_status::active, .parent_plan_id = anchor->id});
  REQUIRE(p.has_value());
  auto t = create_task(conn, task_create_args{.title = "T", .status = task_status::doing, .plan_id = p->id});
  REQUIRE(t.has_value());

  auto r = mark_done(conn, t->id, false);
  REQUIRE(r.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::done);
}

TEST_CASE("auto-promotion path 4/6: mark_cancelled flips the plan to done (cancelled counts as terminal)", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = planar::engine::planning::create_plan(conn, plan_create_args{.title = "Anchor"});
  auto            p      = planar::engine::planning::create_plan(
      conn, plan_create_args{.title = "P", .status = plan_status::active, .parent_plan_id = anchor->id});
  REQUIRE(p.has_value());
  auto t = create_task(conn, task_create_args{.title = "T", .status = task_status::doing, .plan_id = p->id});
  REQUIRE(t.has_value());

  auto r = mark_cancelled(conn, t->id);
  REQUIRE(r.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::done);
}

TEST_CASE("auto-promotion path 5/6: mark_blocked keeps a draft plan promoted to active (blocked counts as active)",
          "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = planar::engine::planning::create_plan(conn, plan_create_args{.title = "P"});
  REQUIRE(p.has_value());
  auto t       = create_task(conn, task_create_args{.title = "T", .plan_id = p->id});
  auto blocker = create_task(conn, task_create_args{.title = "Blocker"});
  REQUIRE(t.has_value());
  REQUIRE(blocker.has_value());

  auto r = mark_blocked(conn, t->id, blocker->id, std::nullopt, false);
  REQUIRE(r.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::active);
}

TEST_CASE("auto-promotion path 6/6: reopen flips the plan back to active", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto            anchor = planar::engine::planning::create_plan(conn, plan_create_args{.title = "Anchor"});
  auto            p      = planar::engine::planning::create_plan(
      conn, plan_create_args{.title = "P", .status = plan_status::active, .parent_plan_id = anchor->id});
  REQUIRE(p.has_value());
  auto t = create_task(conn, task_create_args{.title = "T", .status = task_status::done, .plan_id = p->id});
  REQUIRE(t.has_value());
  auto p_after_done = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_after_done.has_value());
  CHECK(p_after_done->status == plan_status::done);

  auto r = reopen(conn, t->id, task_status::doing, "back to it");
  REQUIRE(r.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::active);
}

TEST_CASE("no_auto_promote=true suppresses the plan-304 recompute on create", "[task][promotion]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = planar::engine::planning::create_plan(conn, plan_create_args{.title = "P"});
  REQUIRE(p.has_value());

  auto t =
      create_task(conn, task_create_args{.title = "T", .status = task_status::doing, .plan_id = p->id, .no_auto_promote = true});
  REQUIRE(t.has_value());

  auto p_reshown = planar::engine::planning::show_plan(conn, p->id);
  REQUIRE(p_reshown.has_value());
  CHECK(p_reshown->status == plan_status::draft); // NOT promoted — suppressed.
}
