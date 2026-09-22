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
TEST_CASE("workflow run start accepts a pid-less run with a lease and lets heartbeat extend it",
          "[runtime][workflowruns][6847]") {
  scratch s;
  auto    c    = open_db(s);
  auto    plan = seed_plan(c);

  auto started = planar::engine::runtime::workflowruns::start(c, {.plan_id        = plan,
                                                                  .pid            = std::nullopt,
                                                                  .ttl_secs       = 8 * 3600,
                                                                  .workflow_name  = "wf",
                                                                  .run_identifier = "lease-a",
                                                                  .repo_root      = "/repo"});
  REQUIRE(started);
  CHECK(started->status == "running");
  CHECK_FALSE(started->pid.has_value());
  REQUIRE(started->expires_at.has_value());

  // Shape check rather than exact-string, since the lease deadline is
  // computed server-side from `now`.
  CHECK(started->expires_at->size() >= 20); // ISO-8601 timestamp shape

  auto const before = *started->expires_at;
  auto       beat   = planar::engine::runtime::workflowruns::heartbeat(c, "lease-a", 9 * 3600);
  REQUIRE(beat);
  CHECK_FALSE(beat->pid.has_value());
  REQUIRE(beat->expires_at.has_value());
  CHECK(*beat->expires_at > before); // extended, not merely re-set to the same instant

  // A row with NEITHER pid nor expires_at is refused at the database's own
  // CHECK — proving the CHECK really is load-bearing for this store, not
  // just for a raw sqlite3 insert.
  CHECK_FALSE(c.execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, repo_root) "
                        "values (" +
                        std::to_string(plan) + ", 'wf', 'neither', '/repo')")
                  .has_value());
}

TEST_CASE("heartbeat refuses a pid-bound run and a non-running run, each by name", "[runtime][workflowruns][6847]") {
  scratch s;
  auto    c    = open_db(s);
  auto    plan = seed_plan(c);

  REQUIRE(planar::engine::runtime::workflowruns::start(
      c, {.plan_id = plan, .pid = 4242, .workflow_name = "wf", .run_identifier = "pid-run", .repo_root = "/repo"}));
  auto on_pid_bound = planar::engine::runtime::workflowruns::heartbeat(c, "pid-run", 600);
  CHECK_FALSE(on_pid_bound);
  CHECK(on_pid_bound.error() == planar::engine::runtime::workflowruns::error::pid_bound);
  // Unchanged: still pid-bound, no expires_at was written.
  CHECK(scalar(c, "select pid from workflow_runs where run_identifier = 'pid-run'") == 4242);
  CHECK(scalar(c, "select expires_at is null from workflow_runs where run_identifier = 'pid-run'") == 1);

  REQUIRE(planar::engine::runtime::workflowruns::start(c, {.plan_id        = plan,
                                                           .pid            = std::nullopt,
                                                           .ttl_secs       = 600,
                                                           .workflow_name  = "wf",
                                                           .run_identifier = "lease-run",
                                                           .repo_root      = "/repo"}));
  REQUIRE(planar::engine::runtime::workflowruns::end(c, "lease-run", "completed"));
  auto on_terminal = planar::engine::runtime::workflowruns::heartbeat(c, "lease-run", 600);
  CHECK_FALSE(on_terminal);
  CHECK(on_terminal.error() == planar::engine::runtime::workflowruns::error::not_running);

  auto on_missing = planar::engine::runtime::workflowruns::heartbeat(c, "does-not-exist", 600);
  CHECK_FALSE(on_missing);
  CHECK(on_missing.error() == planar::engine::runtime::workflowruns::error::run_not_found);
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
