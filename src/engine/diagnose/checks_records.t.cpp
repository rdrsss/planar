// @file checks_records.t.cpp
// @brief Tests for the handoff and sync-conflict checks of `planar.engine.diagnose` (plan 1132, tasks 7378 and 7381).
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
//   * `sync-conflict-unresolved` (state, warning; task 7381): a `sync_events` row with outcome
//     `conflict` and no later event for the same link that ends the conflict: a successful or no-op
//     sync (`ok`, `noop`, `success`; `planar-ext sync resolve` writes an `ok` event) or a
//     `resolved-fs`/`resolved-db` outcome. A later `error` ends nothing; of several conflicts on one
//     link only the latest is reported. Workbench-scope and link-less events are not reported. The
//     fingerprint is the link's (`sync-conflict-unresolved|external_link:<id>|global`), not the event's,
//     so a second conflict on the link is a new occurrence of one incident. The evidence is the event and its link,
//     timed by the event's `at`; the check ignores the window start; the plan scope reaches a link
//     through its task or plan, and a link that belongs to no plan appears only without `--plan`.

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

  /// An external system and `count` links to it: link 1 on task 1 (plan 1), link 2 on task 2 (plan 2),
  /// link 3 on plan 1 itself, link 4 on an artifact (no plan).
  auto links() -> void {
    exec(conn, "insert into external_systems (id, kind, slug, base_url, auth_method, auth_ref) values (1, 'jira', 'jira-demo', "
               "'http://127.0.0.1:1', 'token-env', 'DEMO_TOKEN')");
    exec(conn, "insert into artifacts (id, scope_kind, kind, title, body, created_at, updated_at) values (1, 'global', 'other', "
               "'a', 'b', '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')");
    exec(
        conn,
        "insert into external_links (id, entity_kind, entity_id, system_id, external_id) values "
        "(1, 'task', 1, 1, 'DEMO-1'), (2, 'task', 2, 1, 'DEMO-2'), (3, 'plan', 1, 1, 'DEMO-3'), (4, 'artifact', 1, 1, 'DEMO-4')");
  }

  /// One sync event; `link` 0 is a workbench event with no link.
  auto event(int id, int link, std::string_view outcome, std::string_view at, std::string_view direction = "pull") -> void {
    exec(conn, std::format("insert into sync_events (id, link_id, scope, direction, outcome, at) values ({}, {}, '{}', '{}', "
                           "'{}', '{}')",
                           id, link == 0 ? std::string{"null"} : std::to_string(link), link == 0 ? "workbench" : "external",
                           direction, outcome, at));
  }

  auto run_sync(std::string_view at = k_now, std::optional<int> plan = std::nullopt, std::optional<int> days = std::nullopt)
      -> dg::diagnosis {
    auto result = dg::run(
        conn,
        dg::run_request{.plan_id = plan, .days = days, .checks = {"sync-conflict-unresolved"}, .evaluated_at = std::string{at}});
    REQUIRE(result.has_value());
    return std::move(*result);
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

// ---- sync-conflict-unresolved (plan 1132, task 7381) ----

namespace {

auto primaries_of(const dg::diagnosis& d) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& f : d.findings) {
    out.push_back(im::entity_ref_text(f.primary));
  }
  return out;
}

} // namespace

TEST_CASE("sync-conflict-unresolved is catalogued as a warning state check without inputs", "[engine][diagnose][sync]") {
  auto cat = dg::builtin_catalog();
  auto it  = std::ranges::find(cat.checks, "sync-conflict-unresolved", &dg::check_def::id);
  REQUIRE(it != cat.checks.end());
  CHECK(it->built);
  CHECK(it->kind == im::check_kind::state);
  CHECK(it->severity == im::diagnostic_severity::warning);
  CHECK(it->category == "sync_conflict");
  CHECK(it->inputs.empty());
  CHECK(it->recovery.contains("planar-ext sync resolve"));
}

TEST_CASE("a conflict with no later event is reported with its link and event", "[engine][diagnose][sync]") {
  fixture fx;
  fx.links();
  fx.event(1, 1, "ok", "2026-06-01T08:00:00.000Z");
  fx.event(2, 1, "conflict", "2026-06-01T09:00:00.000Z");
  auto d = fx.run_sync();
  CHECK(d.result == dg::run_outcome::ok);
  REQUIRE(d.findings.size() == 1);
  const auto& f = d.findings[0];
  CHECK(f.check_id == "sync-conflict-unresolved");
  CHECK(f.severity == im::diagnostic_severity::warning);
  CHECK(im::entity_ref_text(f.primary) == "sync_event:2");
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "external_link", .id = 1}));
  CHECK(f.evidence_times == std::vector<std::string>{"2026-06-01T09:00:00.000Z"});
  CHECK(im::finding_fingerprint(f) == "sync-conflict-unresolved|external_link:1|global");
  CHECK(f.recovery.contains("planar-ext sync resolve"));
}

TEST_CASE("a later event that ends the conflict resolves it, and one that does not leaves it", "[engine][diagnose][sync]") {
  // `planar-ext sync resolve` writes an `ok` event; a settled workbench outcome and a clean later sync end it too.
  for (const auto* ends : {"ok", "noop", "success", "resolved-fs", "resolved-db"}) {
    fixture fx;
    fx.links();
    fx.event(1, 1, "conflict", "2026-06-01T09:00:00.000Z");
    fx.event(2, 1, ends, "2026-06-01T09:30:00.000Z", "push");
    INFO(ends);
    CHECK(fx.run_sync().findings.empty());
  }
  // A failed later attempt, or a partial one, ends nothing.
  for (const auto* open : {"error", "partial", "failure", "strategy-abandoned", "counterpart-missing"}) {
    fixture fx;
    fx.links();
    fx.event(1, 1, "conflict", "2026-06-01T09:00:00.000Z");
    fx.event(2, 1, open, "2026-06-01T09:30:00.000Z");
    INFO(open);
    CHECK(primaries_of(fx.run_sync()) == std::vector<std::string>{"sync_event:1"});
  }
}

TEST_CASE("only an event after the conflict, on the same link, resolves it", "[engine][diagnose][sync]") {
  fixture fx;
  fx.links();
  fx.event(1, 1, "ok", "2026-06-01T08:00:00.000Z"); // before the conflict
  fx.event(2, 1, "conflict", "2026-06-01T09:00:00.000Z");
  fx.event(3, 2, "ok", "2026-06-01T09:30:00.000Z"); // another link
  CHECK(primaries_of(fx.run_sync()) == std::vector<std::string>{"sync_event:2"});
}

TEST_CASE("of several conflicts on one link only the latest is reported, and a resolution clears them all",
          "[engine][diagnose][sync]") {
  fixture fx;
  fx.links();
  fx.event(1, 1, "conflict", "2026-06-01T09:00:00.000Z");
  fx.event(2, 1, "conflict", "2026-06-01T09:10:00.000Z");
  CHECK(primaries_of(fx.run_sync()) == std::vector<std::string>{"sync_event:2"});
  fx.event(3, 1, "ok", "2026-06-01T09:20:00.000Z");
  CHECK(fx.run_sync().findings.empty());
  // Two links in conflict are two findings.
  fx.event(4, 2, "conflict", "2026-06-01T10:00:00.000Z");
  fx.event(5, 3, "conflict", "2026-06-01T10:05:00.000Z");
  CHECK(primaries_of(fx.run_sync()) == std::vector<std::string>{"sync_event:4", "sync_event:5"});
}

TEST_CASE("an unresolved conflict is reported whatever its age: the check ignores the window start", "[engine][diagnose][sync]") {
  fixture fx;
  fx.links();
  fx.event(1, 1, "conflict", "2026-01-05T09:00:00.000Z");
  auto narrow = fx.run_sync(k_now, std::nullopt, 1);
  REQUIRE(narrow.findings.size() == 1);
  // The evidence is the event's own row time, so a later evaluation instant changes nothing about it.
  auto later = fx.run_sync("2026-07-10T12:00:00.000Z", std::nullopt, 1);
  REQUIRE(later.findings.size() == 1);
  CHECK(im::finding_digest(narrow.findings[0]) == im::finding_digest(later.findings[0]));
}

TEST_CASE("workbench conflicts and link-less events are not reported: resolve cannot settle them", "[engine][diagnose][sync]") {
  // A workbench sync writes a new conflict row on every run and `sync resolve` refuses a link-less event, so such a
  // row would be a permanent finding. A deleted link nulls `link_id` the same way.
  fixture fx;
  fx.links();
  fx.event(1, 0, "conflict", "2026-06-01T09:00:00.000Z");
  fx.event(2, 0, "conflict", "2026-06-01T09:20:00.000Z");
  fx.event(3, 1, "conflict", "2026-06-01T09:30:00.000Z");
  exec(fx.conn, "update sync_events set scope = 'workbench' where id = 3");
  exec(fx.conn, "insert into sync_events (id, link_id, scope, direction, outcome, at) values (4, null, 'external', 'pull', "
                "'conflict', '2026-06-01T09:40:00.000Z')");
  CHECK(fx.run_sync().findings.empty());
  // The same linked event in the external scope is reported.
  exec(fx.conn, "update sync_events set scope = 'external' where id = 3");
  CHECK(primaries_of(fx.run_sync()) == std::vector<std::string>{"sync_event:3"});
}

TEST_CASE("a second conflict on a link keeps the link's fingerprint and is a new occurrence", "[engine][diagnose][sync]") {
  // Each `sync pull` on a still-conflicting link writes a new conflict event: the incident is the link's, the event is evidence.
  fixture fx;
  fx.links();
  fx.event(1, 1, "conflict", "2026-06-01T09:00:00.000Z");
  auto first = fx.run_sync();
  fx.event(2, 1, "conflict", "2026-06-01T10:00:00.000Z");
  auto second = fx.run_sync();
  REQUIRE(first.findings.size() == 1);
  REQUIRE(second.findings.size() == 1);
  CHECK(im::finding_fingerprint(first.findings[0]) == "sync-conflict-unresolved|external_link:1|global");
  CHECK(im::finding_fingerprint(first.findings[0]) == im::finding_fingerprint(second.findings[0]));
  // The primary entity stays the event (the resolve target) and the newest event's time is the evidence, so the digest moves.
  CHECK(im::entity_ref_text(second.findings[0].primary) == "sync_event:2");
  CHECK(second.findings[0].evidence_times == std::vector<std::string>{"2026-06-01T10:00:00.000Z"});
  CHECK(im::finding_digest(first.findings[0]) != im::finding_digest(second.findings[0]));
  // Two links are two fingerprints.
  fx.event(3, 2, "conflict", "2026-06-01T10:30:00.000Z");
  CHECK(fx.run_sync().findings.size() == 2);
}

TEST_CASE("the recovery hint says to pull again first when the conflict was followed by an error", "[engine][diagnose][sync]") {
  fixture fx;
  fx.links();
  fx.event(1, 1, "conflict", "2026-06-01T09:00:00.000Z");
  fx.event(2, 1, "error", "2026-06-01T09:30:00.000Z");
  auto d = fx.run_sync();
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].recovery.contains("planar-ext sync pull"));
}

TEST_CASE("the plan scope reaches a link through its task or plan", "[engine][diagnose][sync]") {
  fixture fx;
  fx.links();
  fx.event(1, 1, "conflict", "2026-06-01T09:00:00.000Z"); // task 1, plan 1
  fx.event(2, 2, "conflict", "2026-06-01T09:00:00.000Z"); // task 2, plan 2
  fx.event(3, 3, "conflict", "2026-06-01T09:00:00.000Z"); // plan 1 itself
  fx.event(4, 4, "conflict", "2026-06-01T09:00:00.000Z"); // an artifact: no plan
  CHECK(fx.run_sync().findings.size() == 4);
  CHECK(primaries_of(fx.run_sync(k_now, 1)) == std::vector<std::string>{"sync_event:1", "sync_event:3"});
  CHECK(primaries_of(fx.run_sync(k_now, 2)) == std::vector<std::string>{"sync_event:2"});
}
