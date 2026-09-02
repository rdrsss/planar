// Direct lifecycle guards for the harness-owned workflow_runs table.
// These intentionally seed a real migrated database: a test that only
// compares handler exit codes cannot prove a refusal left the row untouched.
#include <catch2/catch_test_macros.hpp>
import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.workflowruns;
namespace {
struct scratch {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      std::format("planar_workflowruns_{}.db", std::chrono::steady_clock::now().time_since_epoch().count());
  ~scratch() {
    std::error_code e;
    std::filesystem::remove(path, e);
    std::filesystem::remove(path.string() + "-wal", e);
    std::filesystem::remove(path.string() + "-shm", e);
  }
};
auto open_db(scratch const& s) -> planar::db::connection {
  auto c = planar::db::connection::open(s.path.string());
  REQUIRE(c);
  REQUIRE(planar::db::apply_all(*c));
  return std::move(*c);
}
auto scalar(planar::db::connection& c, std::string_view sql) -> std::int64_t {
  auto q = c.prepare(sql);
  REQUIRE(q);
  REQUIRE(q->step() == planar::db::step_result::row);
  return q->column_int64(0);
}
auto seed_plan(planar::db::connection& c) -> std::int64_t {
  REQUIRE(c.execute("insert into plans(scope_kind,title,slug,status) values('global','workflow','workflow','active')"));
  return scalar(c, "select last_insert_rowid()");
}
} // namespace
TEST_CASE("workflow run lifecycle writes only running to terminal transitions", "[runtime][workflowruns]") {
  scratch s;
  auto    c       = open_db(s);
  auto    plan    = seed_plan(c);
  auto    started = planar::engine::runtime::workflowruns::start(
      c, {.plan_id = plan, .pid = 42, .workflow_name = "wf", .run_identifier = "run-a", .repo_root = "/repo"});
  REQUIRE(started);
  CHECK(started->status == "running");
  auto ended = planar::engine::runtime::workflowruns::end(c, "run-a", "completed");
  REQUIRE(ended);
  CHECK(ended->status == "completed");
  CHECK(
      scalar(c,
             "select count(*) from workflow_runs where run_identifier='run-a' and status='completed' and ended_at is not null") ==
      1);
  // Break-probe transition guard: a second end must refuse and preserve the
  // original terminal state; removing the engine's running predicate fails.
  auto again = planar::engine::runtime::workflowruns::end(c, "run-a", "failed");
  CHECK_FALSE(again);
  CHECK(again.error() == planar::engine::runtime::workflowruns::error::not_running);
  CHECK(scalar(c, "select count(*) from workflow_runs where run_identifier='run-a' and status='completed'") == 1);
}
TEST_CASE("workflow run start links only existing plans and preserves duplicate refusal", "[runtime][workflowruns]") {
  scratch s;
  auto    c       = open_db(s);
  auto    plan    = seed_plan(c);
  auto    missing = planar::engine::runtime::workflowruns::start(
      c, {.plan_id = 99999, .pid = 1, .workflow_name = "wf", .run_identifier = "missing", .repo_root = "/r"});
  CHECK_FALSE(missing);
  CHECK(missing.error() == planar::engine::runtime::workflowruns::error::plan_not_found);
  CHECK(scalar(c, "select count(*) from workflow_runs") == 0);
  REQUIRE(planar::engine::runtime::workflowruns::start(
      c, {.plan_id = plan, .pid = 1, .workflow_name = "wf", .run_identifier = "duplicate", .repo_root = "/r"}));
  auto duplicate = planar::engine::runtime::workflowruns::start(
      c, {.plan_id = plan, .pid = 2, .workflow_name = "wf", .run_identifier = "duplicate", .repo_root = "/r"});
  CHECK_FALSE(duplicate);
  CHECK(duplicate.error() == planar::engine::runtime::workflowruns::error::query_failed);
  CHECK(scalar(c, "select count(*) from workflow_runs where run_identifier='duplicate'") == 1);
}
