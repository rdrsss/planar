// @file checks_records.t.cpp
// @brief Tests for the handoff check of `planar.engine.diagnose` (plan 1132, task 7378).
//
// The cases drive the shipped catalog over a scratch database with rows written directly, so the
// evaluation instant and the 24 hour boundary are exact. The same check is exercised against
// `planar report`'s own count through the real binaries in
// `src/cmd/planar-watch/diagnose_checks.t.cpp`.
//
// What is pinned:
//
//   * `handoff-stale`: a `pending` or `validated` handoff older than the stale-handoff threshold
//     (24 hours, the one `planar report` and `planar health` apply), measured to the evaluation
//     instant. Exactly at the threshold is not stale.
//   * A `consumed` or `abandoned` handoff is never stale, however old.
//   * The plan scope reaches a handoff through its snapshot's task.
//   * The evidence time is the handoff's `created_at`, and does not move with the instant.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.diagnose;
import planar.incident_model;

namespace {

namespace dg = planar::engine::diagnose;
namespace im = planar::incident_model;

using namespace std::chrono_literals;

constexpr std::string_view k_now = "2026-06-10T12:00:00.000Z";

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_diagnose_records_{}_{}.db",
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

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// The evaluation instant minus `age`, in the stored timestamp format.
auto ago(std::chrono::milliseconds age) -> std::string {
  auto now = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::sys_days{std::chrono::year{2026} / 6 / 10}} + 12h;
  return std::format("{:%FT%T}Z", now - age);
}

struct fixture {
  scratch_db_path        scratch;
  planar::db::connection conn;
  int                    next_snapshot = 1;

  fixture() : conn(open(scratch)) {
    exec(conn, "insert into plans (id, scope_kind, title, slug, created_at) values (1, 'global', 'p1', 'p1', "
               "'2026-01-01T00:00:00.000Z')");
    exec(conn, "insert into plans (id, scope_kind, title, slug, created_at) values (2, 'global', 'p2', 'p2', "
               "'2026-01-01T00:00:00.000Z')");
    exec(conn, "insert into tasks (id, scope_kind, plan_id, title, created_at) values (1, 'global', 1, 't1', "
               "'2026-01-01T00:00:00.000Z')");
    exec(conn, "insert into tasks (id, scope_kind, plan_id, title, created_at) values (2, 'global', 2, 't2', "
               "'2026-01-01T00:00:00.000Z')");
    exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'test', '2026-01-01T00:00:00.000Z')");
  }

  static auto open(const scratch_db_path& scratch) -> planar::db::connection {
    auto conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    return std::move(*conn);
  }

  /// One handoff on a snapshot of `task_id` (0 for none), created `age` before the instant.
  auto handoff(int id, std::string_view status, std::chrono::milliseconds age, int task_id = 1) -> void {
    int snapshot = next_snapshot++;
    exec(conn, std::format("insert into context_snapshots (id, session_id, task_id, vendor, created_at) values ({}, 1, {}, "
                           "'test', '{}')",
                           snapshot, task_id == 0 ? std::string{"null"} : std::to_string(task_id), ago(age)));
    exec(conn, std::format("insert into handoffs (id, from_snapshot_id, from_vendor, status, created_at) values ({}, {}, "
                           "'test', '{}', '{}')",
                           id, snapshot, status, ago(age)));
  }

  auto run(std::string_view at = k_now, std::optional<int> plan = std::nullopt) -> dg::diagnosis {
    auto result = dg::run(
        conn,
        dg::run_request{.plan_id = plan, .days = std::nullopt, .checks = {"handoff-stale"}, .evaluated_at = std::string{at}});
    REQUIRE(result.has_value());
    return std::move(*result);
  }
};

} // namespace

TEST_CASE("handoff-stale is catalogued as a warning state check", "[engine][diagnose][handoff]") {
  auto cat = dg::builtin_catalog();
  auto it  = std::ranges::find(cat.checks, "handoff-stale", &dg::check_def::id);
  REQUIRE(it != cat.checks.end());
  CHECK(it->built);
  CHECK(it->kind == im::check_kind::state);
  CHECK(it->severity == im::diagnostic_severity::warning);
  CHECK(it->category == "handoff_stale");
  CHECK(it->recovery.contains("planar handoff validate"));
}

TEST_CASE("handoff-stale is silent at exactly the threshold and reports just beyond it", "[engine][diagnose][handoff]") {
  fixture at;
  at.handoff(1, "pending", 24h);
  CHECK(at.run().findings.empty());

  fixture beyond;
  beyond.handoff(1, "pending", 24h + 1ms);
  auto d = beyond.run();
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].check_id == "handoff-stale");
  CHECK(d.findings[0].severity == im::diagnostic_severity::warning);
  CHECK(im::entity_ref_text(d.findings[0].primary) == "handoff:1");
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{ago(24h + 1ms)});
  CHECK(d.findings[0].recovery.contains("planar handoff validate"));
}

TEST_CASE("handoff-stale reads pending and validated handoffs and never a consumed or abandoned one",
          "[engine][diagnose][handoff]") {
  fixture fx;
  fx.handoff(1, "pending", 30h);
  fx.handoff(2, "validated", 30h);
  fx.handoff(3, "consumed", 30h);
  fx.handoff(4, "abandoned", 30h);
  fx.handoff(5, "validated", 3h);
  auto d = fx.run();
  REQUIRE(d.findings.size() == 2);
  CHECK(im::entity_ref_text(d.findings[0].primary) == "handoff:1");
  CHECK(im::entity_ref_text(d.findings[1].primary) == "handoff:2");
}

TEST_CASE("handoff-stale is measured to the evaluation instant, not the clock", "[engine][diagnose][handoff]") {
  fixture fx;
  fx.handoff(1, "validated", 3h);
  CHECK(fx.run(k_now).findings.empty());
  auto later = fx.run("2026-06-11T12:00:00.000Z");
  REQUIRE(later.findings.size() == 1);
  // The evidence is the handoff's own row time at either instant.
  CHECK(later.findings[0].evidence_times == std::vector<std::string>{ago(3h)});
}

TEST_CASE("handoff-stale follows the plan scope through the snapshot's task", "[engine][diagnose][handoff]") {
  fixture fx;
  fx.handoff(1, "pending", 30h, 1);
  fx.handoff(2, "pending", 30h, 2);
  fx.handoff(3, "pending", 30h, 0);
  CHECK(fx.run().findings.size() == 3);
  auto scoped = fx.run(k_now, 1);
  REQUIRE(scoped.findings.size() == 1);
  CHECK(im::entity_ref_text(scoped.findings[0].primary) == "handoff:1");
  CHECK(std::ranges::contains(scoped.findings[0].evidence, im::entity_ref{.kind = "task", .id = 1}));
}
