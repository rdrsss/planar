// @file scope_ref_agreement.t.cpp
// @brief Cross-consumer agreement test for `planar.scope_ref` (plan 996,
// task 6089, decision D19).
//
// D19's whole point is that `engine_identity` and `engine_planning` must
// resolve the SAME scope-ref slug to the SAME `(kind, id)` pair, via the
// SAME extracted implementation (`planar.scope_ref::resolve`), instead of
// two independently-maintained copies that can silently drift. This test
// proves that BY CONSTRUCTION: for every accepted scope-ref form, it
// resolves the slug directly through `engine_identity::resolve_slug` AND
// indirectly through `engine_planning::create_plan`/`create_task` (whose
// `resolve_scope_or_global` now delegates to the same `scope_ref::resolve`
// — see plan.cpp/task.cpp), then asserts the two outcomes name the exact
// same scope row.
//
// This is a genuine regression detector, not a tautology: it exercises
// each consumer's OWN translation layer (`engine_identity`'s
// `to_local_kind`/`to_local_error`, `engine_planning`'s
// `to_plan_kind`/`to_task_kind`) independently, at the public-API level,
// so a bug introduced in either translation — even though both still call
// the one shared `resolve()` underneath — trips this test. Break-probe
// (task 6089 coder report): temporarily swapping `association`/`repo` in
// `plan.cpp`'s `to_plan_kind` made this test's "assoc:acme" case fail
// (expected association/1, observed repo-shaped mismatch), confirming the
// test is not vacuous; the swap was reverted before commit.
//
// `engine_identity` is a TEST-ONLY dependency of this module (see this
// module's CMakeLists.txt) — no production `.cpp` here names
// `planar.engine.identity` at all, and D18's same-layer prohibition is
// therefore untouched by this test binary.
//
// Include-before-import is deliberate (see db/db.t.cpp): MSVC's supported
// direction for mixing textual std headers with IFC imports is
// include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.identity.scope;
import planar.engine.planning.plan;
import planar.engine.planning.task;

namespace {

namespace identity = planar::engine::identity;
namespace planning = planar::engine::planning;

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Mirrors
/// identity/scope.t.cpp's identical helper — duplicated rather than
/// shared because this codebase has no header tree for first-party code.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_scope_ref_agreement_test_{}_{}.db",
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

auto insert_project(planar::db::connection& conn, std::string_view slug, std::string_view root_path) -> std::int64_t {
  auto stmt = conn.prepare("insert into projects (slug, name, root_path) values (?, ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->bind_text(3, root_path).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_association(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into associations (slug, name, kind) values (?, ?, 'org') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief True when `pk` names the same scope kind as `ik`. The two enums
/// are deliberately distinct types with DIFFERENT declaration order
/// (`identity::scope_kind` is global/association/repo;
/// `planning::plan_scope_kind`/`task_scope_kind` are repo/association/
/// global — see plan.cppm's file comment), so this maps by NAME, not by
/// ordinal, which is exactly the kind of drift a naive `static_cast`
/// comparison would miss.
auto kinds_agree(identity::scope_kind ik, planning::plan_scope_kind pk) -> bool {
  switch (ik) {
  case identity::scope_kind::global:
    return pk == planning::plan_scope_kind::global;
  case identity::scope_kind::association:
    return pk == planning::plan_scope_kind::association;
  case identity::scope_kind::repo:
    return pk == planning::plan_scope_kind::repo;
  }
  return false;
}

auto kinds_agree(identity::scope_kind ik, planning::task_scope_kind tk) -> bool {
  switch (ik) {
  case identity::scope_kind::global:
    return tk == planning::task_scope_kind::global;
  case identity::scope_kind::association:
    return tk == planning::task_scope_kind::association;
  case identity::scope_kind::repo:
    return tk == planning::task_scope_kind::repo;
  }
  return false;
}

} // namespace

TEST_CASE("engine_identity and engine_planning agree on every scope-ref form via create_plan", "[scope_ref][agreement][plan]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      assoc_id   = insert_association(conn, "acme");
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");

  for (const std::string_view slug : {"global", "acme", "assoc:acme", "repo:myrepo"}) {
    auto direct = identity::resolve_slug(conn, slug);
    REQUIRE(direct.has_value());

    auto plan = planning::create_plan(
        conn, planning::plan_create_args{.title = std::format("plan for {}", slug), .scope = std::string(slug)});
    REQUIRE(plan.has_value());

    CHECK(kinds_agree(direct->kind, plan->scope_kind));
    CHECK(plan->scope_id == direct->id);

    // Sanity: the ids actually landed, not just "both unset".
    if (slug == "acme" || slug == "assoc:acme") {
      REQUIRE(plan->scope_id.has_value());
      CHECK(*plan->scope_id == assoc_id);
    } else if (slug == "repo:myrepo") {
      REQUIRE(plan->scope_id.has_value());
      CHECK(*plan->scope_id == project_id);
    } else {
      CHECK_FALSE(plan->scope_id.has_value());
    }
  }
}

TEST_CASE("engine_identity and engine_planning agree on every scope-ref form via create_task", "[scope_ref][agreement][task]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      assoc_id   = insert_association(conn, "acme");
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");

  for (const std::string_view slug : {"global", "acme", "assoc:acme", "repo:myrepo"}) {
    auto direct = identity::resolve_slug(conn, slug);
    REQUIRE(direct.has_value());

    auto task = planning::create_task(
        conn, planning::task_create_args{.title = std::format("task for {}", slug), .scope = std::string(slug)});
    REQUIRE(task.has_value());

    CHECK(kinds_agree(direct->kind, task->scope_kind));
    CHECK(task->scope_id == direct->id);

    if (slug == "acme" || slug == "assoc:acme") {
      REQUIRE(task->scope_id.has_value());
      CHECK(*task->scope_id == assoc_id);
    } else if (slug == "repo:myrepo") {
      REQUIRE(task->scope_id.has_value());
      CHECK(*task->scope_id == project_id);
    } else {
      CHECK_FALSE(task->scope_id.has_value());
    }
  }
}

TEST_CASE("engine_identity and engine_planning agree on an unknown scope-ref slug: both refuse",
          "[scope_ref][agreement][error]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto direct = identity::resolve_slug(conn, "no-such-slug");
  REQUIRE_FALSE(direct.has_value());
  CHECK(direct.error() == identity::scope_error::slug_not_found);

  auto plan = planning::create_plan(conn, planning::plan_create_args{.title = "orphan plan", .scope = "no-such-slug"});
  REQUIRE_FALSE(plan.has_value());
  CHECK(plan.error() == planning::plan_error::slug_not_found);

  auto task = planning::create_task(conn, planning::task_create_args{.title = "orphan task", .scope = "no-such-slug"});
  REQUIRE_FALSE(task.has_value());
  CHECK(task.error() == planning::task_error::slug_not_found);
}
