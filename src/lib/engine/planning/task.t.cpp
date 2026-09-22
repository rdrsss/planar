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

TEST_CASE("update: a task with an entity annotation cannot change scope", "[task][entity-annotation][scope]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "Anchored"});
  REQUIRE(t.has_value());
  REQUIRE(conn.execute("insert into associations (slug, name, kind) values ('destination', 'destination', 'org')"));
  REQUIRE(conn.execute(std::format("insert into annotations "
                                   "(scope_kind, anchor_kind, target_kind, target_id, body, task_id) "
                                   "values ('global', 'entity', 'task', {}, 'durable note', {})",
                                   t->id, t->id)));

  auto moved = update_task(conn, t->id, task_update_args{.scope = "assoc:destination"});
  REQUIRE_FALSE(moved.has_value());
  CHECK(moved.error() == task_error::query_failed);
  auto unchanged = show_task(conn, t->id);
  REQUIRE(unchanged.has_value());
  CHECK(unchanged->scope_kind == planar::engine::planning::task_scope_kind::global);
  CHECK_FALSE(unchanged->scope_id.has_value());
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

// ===========================================================================
// task 6135 — `due_at` validation and the two renderers
// ===========================================================================

TEST_CASE("due_at accepts date-only and RFC3339, rejects everything else", "[task][due][6135]") {
  // Ported from zig/src/engine/planning/task.zig's `parseDueAt` when `task
  // add` was wired and brought a `--due` flag with it. The original C++
  // port recorded this as a `cmd/`-layer concern and skipped it; the oracle
  // validates in the ENGINE, and the difference is observable — without
  // this check `task add --due not-a-date` exits 0 and stores garbage.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  for (auto const& good : {
           "",                                 // accepted UNCHANGED, not rejected
           "2026-09-01",                       // date only
           "2024-02-29",                       // real leap day
           "2026-09-01T10:00:00Z",             // Z-terminated
           "2026-09-01T10:00:00.123Z",         // fractional + Z
           "2026-09-01T10:00:00+05:30",        // offset
           "2026-09-01T10:00:00.000001-08:00", // long fraction + negative offset
       }) {
    INFO("due_at " << good);
    auto t = create_task(conn, task_create_args{.title = "T", .due_at = std::string{good}});
    REQUIRE(t.has_value());
    REQUIRE(t->due_at.has_value());
    CHECK(*t->due_at == good); // stored verbatim, never reformatted
  }

  for (auto const& bad : {
           "not-a-date",
           "2026-9-1",                   // unpadded
           "2026-02-30",                 // calendar-invalid, not merely range-checked
           "2023-02-29",                 // not a leap year
           "2026-13-01",                 // month out of range
           "2026-00-01",                 // month zero
           "2026-09-00",                 // day zero
           "2026-09-01T25:00:00Z",       // hour out of range
           "2026-09-01T10:60:00Z",       // minute out of range
           "2026-09-01T10:00:60Z",       // second out of range
           "2026-09-01T10:00:00",        // no zone at all
           "2026-09-01T10:00:00Z ",      // trailing space AFTER the Z
           "2026-09-01T10:00:00.Z",      // empty fraction
           "2026-09-01T10:00:00+5:30",   // unpadded offset
           "2026-09-01T10:00:00+05:30x", // trailing junk
           "2026-09-01T10:00:00+24:00",  // offset hour out of range
           " 2026-09-01",                // leading space
       }) {
    INFO("due_at " << bad);
    auto t = create_task(conn, task_create_args{.title = "T", .due_at = std::string{bad}});
    REQUIRE_FALSE(t.has_value());
    CHECK(t.error() == task_error::invalid_due_at);
  }
}

TEST_CASE("due_at validation runs AFTER scope resolution", "[task][due][6135]") {
  // Both failures surface at exit 1 through the handler, so only the
  // ERROR distinguishes them — and the oracle resolves the scope slug
  // first (task.zig:309) and validates the due date second (:323).
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            both = create_task(conn, task_create_args{.title = "T", .due_at = "garbage", .scope = "nosuchscope"});
  REQUIRE_FALSE(both.has_value());
  CHECK(both.error() == task_error::slug_not_found);
}

TEST_CASE("update also rejects a malformed due_at", "[task][due][6135]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "T"});
  REQUIRE(t.has_value());

  auto bad = update_task(conn, t->id, task_update_args{.due_at = "nope"});
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error() == task_error::invalid_due_at);

  auto good = update_task(conn, t->id, task_update_args{.due_at = "2026-12-31"});
  REQUIRE(good.has_value());
  CHECK(good->due_at == "2026-12-31");
}

TEST_CASE("render_text omits every unset optional and clamps a negative priority", "[task][render][6135]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = "Bare", .priority = -5});
  REQUIRE(t.has_value());

  auto const text = planar::engine::planning::render_text(*t);
  // Thirteen-column labels. Five conditional lines, all absent here.
  CHECK(text == std::format("id:          {}\n"
                            "title:       Bare\n"
                            "status:      todo\n"
                            "priority:    0\n" // CLAMPED; the row holds -5
                            "scope:       global\n"
                            "created:     {}\n"
                            "updated:     {}\n",
                            t->id, t->created_at, t->updated_at));
  CHECK(t->priority == -5); // the clamp is presentation only
  // `slug` has no text line at all even when set — it surfaces only in JSON.
  auto slugged = create_task(conn, task_create_args{.title = "Slugged", .slug = "s-1"});
  REQUIRE(slugged.has_value());
  CHECK_FALSE(planar::engine::planning::render_text(*slugged).contains("s-1"));
}

TEST_CASE("render_text prints the five conditional lines in the oracle's order", "[task][render][6135]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            p    = planar::engine::planning::create_plan(conn, plan_create_args{.title = "P"});
  REQUIRE(p.has_value());
  auto parent = create_task(conn, task_create_args{.title = "Parent"});
  REQUIRE(parent.has_value());
  auto t = create_task(conn, task_create_args{.title          = "Full",
                                              .body           = "B",
                                              .priority       = 3,
                                              .plan_id        = p->id,
                                              .parent_task_id = parent->id,
                                              .next_action    = "NA",
                                              .due_at         = "2026-09-01"});
  REQUIRE(t.has_value());

  auto const text = planar::engine::planning::render_text(*t);
  // plan, parent, next action, due, body — between `scope` and `created`.
  CHECK(text.contains("scope:       global\n"
                      "plan:        " +
                      std::format("{}\n", p->id) + "parent:      " + std::format("{}\n", parent->id) +
                      "next action: NA\n"
                      "due:         2026-09-01\n"
                      "body:        B\n"
                      "created:     "));
  CHECK(text.ends_with("\n"));
}

TEST_CASE("render_json emits declaration order, nulls, and an UNCLAMPED priority", "[task][render][6135]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            t    = create_task(conn, task_create_args{.title = R"(quote " and \ back)", .priority = -5});
  REQUIRE(t.has_value());

  auto const json = planar::engine::planning::render_json(*t);
  CHECK(json == std::format(R"({{"id":{},"scope_kind":"global","scope_id":null,"plan_id":null,)"
                            R"("parent_task_id":null,"title":"quote \" and \\ back","body":null,"slug":null,)"
                            R"("status":"todo","priority":-5,"next_action":null,"due_at":null,)"
                            R"("created_at":"{}","updated_at":"{}"}})",
                            t->id, t->created_at, t->updated_at));
  // A fragment: the CALLER terminates it. `render_text` does not.
  CHECK_FALSE(json.ends_with("\n"));
  CHECK(planar::engine::planning::render_text(*t).ends_with("\n"));
}

// --- task 6754 / decision 1122: auto-clearing an unblocked dependent ------

TEST_CASE("completing the LAST blocker clears the dependent; a partial clear does not", "[task][verbs][6754]") {
  // Both halves in ONE case deliberately: asserted separately, a
  // partial-clearance no-op and a full-clearance transition could each pass
  // against an implementation that got the other backwards. The dependent
  // must stay `blocked` after the first blocker completes and move to
  // `todo` only after the second.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            dep  = create_task(conn, task_create_args{.title = "Dependent"});
  auto            b1   = create_task(conn, task_create_args{.title = "Blocker one", .status = task_status::doing});
  auto            b2   = create_task(conn, task_create_args{.title = "Blocker two", .status = task_status::doing});
  REQUIRE(dep.has_value());
  REQUIRE(b1.has_value());
  REQUIRE(b2.has_value());

  REQUIRE(mark_blocked(conn, dep->id, b1->id, std::nullopt, false).has_value());
  REQUIRE(mark_blocked(conn, dep->id, b2->id, std::nullopt, false).has_value());

  // One of two done: still blocked. This is the arm that fails if the
  // all-clear rule degrades to any-clear.
  REQUIRE(mark_done(conn, b1->id, false).has_value());
  auto after_first = show_task(conn, dep->id);
  REQUIRE(after_first.has_value());
  CHECK(after_first->status == task_status::blocked);

  // Both done: cleared.
  REQUIRE(mark_done(conn, b2->id, false).has_value());
  auto after_second = show_task(conn, dep->id);
  REQUIRE(after_second.has_value());
  CHECK(after_second->status == task_status::todo);
}

TEST_CASE("a CANCELLED blocker clears its dependent, exactly as a done one does", "[task][verbs][6754]") {
  // `docs/concepts.md`'s closeout gate states cancelled tasks are terminal
  // and do not block; `is_terminal` agrees. A dependent left blocked behind
  // a cancelled blocker would be permanently stuck, since nothing will ever
  // complete it.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            dep  = create_task(conn, task_create_args{.title = "Dependent"});
  auto            b    = create_task(conn, task_create_args{.title = "Blocker", .status = task_status::doing});
  REQUIRE(dep.has_value());
  REQUIRE(b.has_value());
  REQUIRE(mark_blocked(conn, dep->id, b->id, std::nullopt, false).has_value());

  REQUIRE(mark_cancelled(conn, b->id).has_value());
  auto after = show_task(conn, dep->id);
  REQUIRE(after.has_value());
  CHECK(after->status == task_status::todo);
}

TEST_CASE("task update --status done clears dependents too, not only mark_done", "[task][verbs][6754]") {
  // `update_task` writes `status` in its own UPDATE rather than through
  // `set_status`, so it is a THIRD path to terminal. Wiring only
  // mark_done/mark_cancelled would make the auto-clear fire on two of three
  // paths and look arbitrary.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            dep  = create_task(conn, task_create_args{.title = "Dependent"});
  auto            b    = create_task(conn, task_create_args{.title = "Blocker", .status = task_status::doing});
  REQUIRE(dep.has_value());
  REQUIRE(b.has_value());
  REQUIRE(mark_blocked(conn, dep->id, b->id, std::nullopt, false).has_value());

  REQUIRE(update_task(conn, b->id, task_update_args{.status = task_status::done}).has_value());
  auto after = show_task(conn, dep->id);
  REQUIRE(after.has_value());
  CHECK(after->status == task_status::todo);
}

TEST_CASE("auto-clearing leaves a distinguishable audit summary", "[task][verbs][6754]") {
  // An auto-transition and an operator's own `blocked -> todo` must not be
  // indistinguishable after the fact -- decision 1122 requires the source be
  // legible in the trail.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            dep  = create_task(conn, task_create_args{.title = "Dependent"});
  auto            b    = create_task(conn, task_create_args{.title = "Blocker", .status = task_status::doing});
  REQUIRE(dep.has_value());
  REQUIRE(b.has_value());
  REQUIRE(mark_blocked(conn, dep->id, b->id, std::nullopt, false).has_value());
  REQUIRE(mark_done(conn, b->id, false).has_value());

  auto stmt = conn.prepare("select summary from audit_log where entity_kind='task' and entity_id=? "
                           "order by id desc limit 1");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, dep->id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  CHECK(stmt->column_text(0) == std::format("unblocked: task {} is terminal", b->id));
}

TEST_CASE("a non-blocked dependent is left alone when its blocker completes", "[task][verbs][6754]") {
  // The status guard: only a row actually AT `blocked` is touched. A
  // dependent an operator already moved to `doing` must not be yanked back
  // to `todo` by its blocker completing.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            dep  = create_task(conn, task_create_args{.title = "Dependent"});
  auto            b    = create_task(conn, task_create_args{.title = "Blocker", .status = task_status::doing});
  REQUIRE(dep.has_value());
  REQUIRE(b.has_value());
  REQUIRE(mark_blocked(conn, dep->id, b->id, std::nullopt, false).has_value());
  REQUIRE(update_task(conn, dep->id, task_update_args{.status = task_status::doing}).has_value());

  REQUIRE(mark_done(conn, b->id, false).has_value());
  auto after = show_task(conn, dep->id);
  REQUIRE(after.has_value());
  CHECK(after->status == task_status::doing);
}

TEST_CASE("update_task reports busy_source, not query_failed, when a competing writer "
          "holds the write lock past busy_timeout",
          "[task][busy][6843]") {
  // Mirrors db.t.cpp's "begin_transaction(lock_mode::immediate) takes the
  // write lock synchronously" contention shape: a second connection takes
  // an IMMEDIATE lock (the RESERVED write lock, synchronously, before any
  // statement runs) and holds it, while the connection under test has its
  // busy_timeout lowered so the case does not wait out the real 5000ms
  // default (task 6842).
  scratch_db_path scratch;
  auto            conn_a = open_migrated(scratch);

  auto conn_b = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn_b.has_value());
  REQUIRE(conn_b->execute("pragma busy_timeout = 50;"));

  auto created = create_task(conn_a, task_create_args{.title = "Contended"});
  REQUIRE(created.has_value());

  auto locker = conn_a.begin_transaction(planar::db::lock_mode::immediate);
  REQUIRE(locker.has_value());

  auto updated = update_task(*conn_b, created->id, task_update_args{.title = "New title"});
  REQUIRE_FALSE(updated.has_value());
  CHECK(updated.error() == task_error::busy_source);

  REQUIRE(locker->commit().has_value());

  // Confirms the write genuinely never landed while contended, and that a
  // retry (no longer contended) succeeds cleanly.
  auto after = show_task(*conn_b, created->id);
  REQUIRE(after.has_value());
  CHECK(after->title == "Contended");

  auto retried = update_task(*conn_b, created->id, task_update_args{.title = "New title"});
  REQUIRE(retried.has_value());
  CHECK(retried->title == "New title");
}
