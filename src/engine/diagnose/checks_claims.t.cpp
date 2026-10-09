// @file checks_claims.t.cpp
// @brief Tests for the claim-liveness checks of `planar.engine.diagnose` (plan 1132, task 7374).
//
// These cases drive the shipped catalog over a scratch database with rows written directly, so
// the evaluation instant and every boundary are exact. The same checks are exercised through the
// real `planar-agent` verbs in `src/cmd/planar-watch/diagnose_checks.t.cpp`; the claim lifecycle
// is written there by the verbs themselves, and only timestamp columns are moved afterwards.
//
// What is pinned:
//
//   * `claim-lease-lapsed`: an active claim past its lease that heartbeated, on a `doing` task.
//   * `claim-process-died`: an active claim past its lease that never heartbeated after claiming.
//   * `claim-superseded-active`: an active claim on a terminal entity, or behind a later claim
//     that is no longer active.
//   * `task-doing-unclaimed`: a `doing` task with no active, unexpired claim.
//   * `claim-closed-by-reconcile`: a claim that ended `stale`, inside the window.
//   * A healthy fixture yields no finding from any claim check.
//   * Evidence times are row timestamps: moving the evaluation instant does not move them.
//   * `heartbeat-gap` (task 7375, decisions 1345 and 1381): the points of a claim are `claimed_at`,
//     each heartbeat action row and `last_heartbeat_at`. A gap between two points is `info` when
//     strictly beyond half the lease and never more; a `warning` is the trailing stretch from the
//     last point to the release (terminal claim) or the evaluation instant (active claim) strictly
//     beyond the lease. The lease is `lease_expires_at - last_heartbeat_at`.
//   * `claim-superseded-active` evidence (claim, entity, later ended claim, the lease expiry), the
//     exclusive-only rule, and the plan and plan-step branches; `task-doing-unclaimed` times its
//     evidence from the latest claim, not from the task's `updated_at`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.diagnose;
import planar.incident_model;

namespace {

namespace dg = planar::engine::diagnose;
namespace im = planar::incident_model;

constexpr std::string_view k_now = "2026-06-01T12:00:00.000Z";

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_diagnose_claims_{}_{}.db",
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

struct fixture {
  scratch_db_path        scratch;
  planar::db::connection conn;

  fixture() : conn(open(scratch)) {
    exec(conn, "insert into plans (id, scope_kind, title, slug, created_at) values (1, 'global', 'p1', 'p1', "
               "'2026-01-01T00:00:00.000Z')");
    exec(conn, "insert into plans (id, scope_kind, title, slug, created_at) values (2, 'global', 'p2', 'p2', "
               "'2026-01-01T00:00:00.000Z')");
    exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'test', '2026-01-01T00:00:00.000Z')");
  }

  static auto open(const scratch_db_path& scratch) -> planar::db::connection {
    auto conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    return std::move(*conn);
  }

  auto task(int id, std::string_view status, int plan = 1) -> void {
    exec(conn, std::format("insert into tasks (id, scope_kind, plan_id, title, status, created_at, updated_at) values ({}, "
                           "'global', {}, 't{}', '{}', '2026-05-01T00:00:00.000Z', '2026-05-02T00:00:00.000Z')",
                           id, plan, id, status));
  }

  /// One claim row on any entity kind (`task`, `plan` or `plan_step`) and claim scope.
  auto entity_claim(int id, std::string_view kind, int entity_id, std::string_view scope, std::string_view status,
                    std::string_view claimed, std::string_view last_heartbeat, std::string_view lease_expires,
                    std::string_view released = {}) -> void {
    exec(conn, std::format("insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, "
                           "status, vendor, claimed_at, last_heartbeat_at, lease_expires_at, released_at) values ({}, 'tok{}', "
                           "1, '{}', {}, '{}', '{}', 'test', '{}', '{}', '{}', {})",
                           id, id, kind, entity_id, scope, status, claimed, last_heartbeat, lease_expires,
                           released.empty() ? std::string{"null"} : std::format("'{}'", released)));
  }

  /// One exclusive claim row on a task. `released` is empty for an unended claim.
  auto claim(int id, int task_id, std::string_view status, std::string_view claimed, std::string_view last_heartbeat,
             std::string_view lease_expires, std::string_view released = {}) -> void {
    entity_claim(id, "task", task_id, "exclusive", status, claimed, last_heartbeat, lease_expires, released);
  }

  auto set_plan_status(int id, std::string_view status) -> void {
    exec(conn, std::format("update plans set status = '{}' where id = {}", status, id));
  }

  auto step(int id, int plan, std::string_view status) -> void {
    exec(conn, std::format("insert into plan_steps (id, plan_id, ordinal, body, status) values ({}, {}, {}, 'step', '{}')", id,
                           plan, id, status));
  }

  auto heartbeat_action(int id, int claim_id, std::string_view at) -> void {
    exec(conn, std::format("insert into agent_actions (id, session_id, claim_id, action_kind, vendor, started_at, ended_at, "
                           "outcome) values ({}, 1, {}, 'heartbeat', 'test', '{}', '{}', 'ok')",
                           id, claim_id, at, at));
  }

  auto run(std::vector<std::string> checks, std::string_view at = k_now, std::optional<int> plan = std::nullopt,
           std::optional<int> days = std::nullopt) -> dg::diagnosis {
    auto result = dg::run(
        conn, dg::run_request{.plan_id = plan, .days = days, .checks = std::move(checks), .evaluated_at = std::string{at}});
    REQUIRE(result.has_value());
    return std::move(*result);
  }
};

auto ids_of(const dg::diagnosis& d) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& f : d.findings) {
    out.push_back(std::format("{} {}", f.check_id, im::entity_ref_text(f.primary)));
  }
  return out;
}

const std::vector<std::string> k_claim_checks{"claim-lease-lapsed", "claim-process-died", "claim-superseded-active",
                                              "task-doing-unclaimed", "claim-closed-by-reconcile"};

} // namespace

TEST_CASE("the claim checks are catalogued with the spec's kind, severity and category", "[engine][diagnose][claims]") {
  auto cat = dg::builtin_catalog();
  struct row {
    std::string_view        id;
    im::check_kind          kind;
    im::diagnostic_severity severity;
    std::string_view        category;
  };
  for (const auto& want : std::vector<row>{
           {"claim-lease-lapsed", im::check_kind::state, im::diagnostic_severity::warning, "claim_lease_lapsed"},
           {"claim-process-died", im::check_kind::state, im::diagnostic_severity::warning, "claim_process_died"},
           {"claim-superseded-active", im::check_kind::state, im::diagnostic_severity::error, "claim_superseded_active"},
           {"task-doing-unclaimed", im::check_kind::state, im::diagnostic_severity::warning, "claim_doing_unclaimed"},
           {"claim-closed-by-reconcile", im::check_kind::event, im::diagnostic_severity::info, "claim_closed_by_reconcile"}}) {
    auto it = std::ranges::find(cat.checks, want.id, &dg::check_def::id);
    INFO(want.id);
    REQUIRE(it != cat.checks.end());
    CHECK(it->built);
    CHECK(it->kind == want.kind);
    CHECK(it->severity == want.severity);
    CHECK(it->category == want.category);
  }
}

TEST_CASE("claim-lease-lapsed reports an active, heartbeated claim past its lease on a doing task",
          "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T09:40:00.000Z");
  fx.heartbeat_action(1, 1, "2026-06-01T09:30:00.000Z");

  auto d = fx.run({"claim-lease-lapsed"});
  REQUIRE(d.findings.size() == 1);
  const auto& f = d.findings[0];
  CHECK(f.check_id == "claim-lease-lapsed");
  CHECK(f.severity == im::diagnostic_severity::warning);
  CHECK(im::entity_ref_text(f.primary) == "claim:1");
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "task", .id = 1}));
  CHECK(f.evidence_times == std::vector<std::string>{"2026-06-01T09:40:00.000Z"});
  CHECK(f.recovery.contains("planar-agent reconcile"));

  // A claim that heartbeated is not a dead process.
  CHECK(fx.run({"claim-process-died"}).findings.empty());
}

TEST_CASE("a heartbeat action row alone counts as a heartbeat after claiming", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:40:00.000Z");
  fx.heartbeat_action(1, 1, "2026-06-01T09:20:00.000Z");
  CHECK(ids_of(fx.run({"claim-lease-lapsed", "claim-process-died"})) == std::vector<std::string>{"claim-lease-lapsed claim:1"});
}

TEST_CASE("a last_heartbeat_at past claimed_at alone counts as a heartbeat", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:20:00.000Z", "2026-06-01T09:40:00.000Z");
  CHECK(ids_of(fx.run({"claim-lease-lapsed", "claim-process-died"})) == std::vector<std::string>{"claim-lease-lapsed claim:1"});
}

TEST_CASE("a freshly taken claim that has not heartbeated yet is not a dead process", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T11:58:00.000Z", "2026-06-01T11:58:00.000Z", "2026-06-01T12:08:00.000Z");
  CHECK(fx.run(k_claim_checks).findings.empty());
}

TEST_CASE("claim-lease-lapsed treats a lease expiring exactly now as still live", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:00:00.000Z", std::string{k_now});
  fx.heartbeat_action(1, 1, "2026-06-01T11:00:00.000Z");
  CHECK(fx.run({"claim-lease-lapsed", "task-doing-unclaimed"}).findings.empty());
  CHECK(fx.run({"claim-lease-lapsed"}, "2026-06-01T12:00:00.001Z").findings.size() == 1);
}

TEST_CASE("claim-lease-lapsed ignores a lapsed claim whose task is no longer doing", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "todo");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T09:40:00.000Z");
  fx.heartbeat_action(1, 1, "2026-06-01T09:30:00.000Z");
  CHECK(fx.run({"claim-lease-lapsed"}).findings.empty());
}

TEST_CASE("claim-process-died reports a lapsed claim that never heartbeated after claiming", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z");

  auto d = fx.run({"claim-process-died"});
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].check_id == "claim-process-died");
  CHECK(d.findings[0].severity == im::diagnostic_severity::warning);
  CHECK(im::entity_ref_text(d.findings[0].primary) == "claim:1");
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{"2026-06-01T09:10:00.000Z"});
  CHECK(fx.run({"claim-lease-lapsed"}).findings.empty());
}

TEST_CASE("claim-process-died needs no doing task and is silent once a terminal verb ran", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "todo");
  fx.task(2, "done");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z");
  fx.claim(2, 2, "completed", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z",
           "2026-06-01T09:05:00.000Z");
  auto d = fx.run({"claim-process-died"});
  CHECK(ids_of(d) == std::vector<std::string>{"claim-process-died claim:1"});
}

TEST_CASE("claim-superseded-active reports an active claim on a terminal task", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "done");
  fx.task(2, "cancelled");
  fx.task(3, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  fx.claim(2, 2, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  fx.claim(3, 3, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");

  auto d = fx.run({"claim-superseded-active"});
  REQUIRE(d.findings.size() == 2);
  CHECK(ids_of(d) == std::vector<std::string>{"claim-superseded-active claim:1", "claim-superseded-active claim:2"});
  for (const auto& f : d.findings) {
    CHECK(f.severity == im::diagnostic_severity::error);
    CHECK(f.recovery.contains("planar-agent abort --claim"));
  }
  // The evidence is the claim, its task and the active claim's own lease expiry.
  CHECK(d.findings[0].evidence ==
        std::vector<im::entity_ref>{im::entity_ref{.kind = "claim", .id = 1}, im::entity_ref{.kind = "task", .id = 1}});
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{"2026-06-01T12:05:00.000Z"});
}

TEST_CASE("claim-superseded-active reports an active claim behind a later claim that ended", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  fx.claim(2, 1, "aborted", "2026-06-01T10:00:00.000Z", "2026-06-01T10:00:00.000Z", "2026-06-01T10:10:00.000Z",
           "2026-06-01T10:05:00.000Z");
  auto d = fx.run({"claim-superseded-active"});
  CHECK(ids_of(d) == std::vector<std::string>{"claim-superseded-active claim:1"});
  // The evidence names the active claim, its task and the later ended claim, and times the lease.
  REQUIRE(d.findings.size() == 1);
  CHECK(std::ranges::contains(d.findings[0].evidence, im::entity_ref{.kind = "claim", .id = 1}));
  CHECK(std::ranges::contains(d.findings[0].evidence, im::entity_ref{.kind = "task", .id = 1}));
  CHECK(std::ranges::contains(d.findings[0].evidence, im::entity_ref{.kind = "claim", .id = 2}));
  CHECK(d.findings[0].evidence.size() == 3);
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{"2026-06-01T12:05:00.000Z", "2026-06-01T10:05:00.000Z"});
  // The fingerprint names the active claim and its entity only.
  CHECK(im::finding_fingerprint(d.findings[0]) == "claim-superseded-active|claim:1|task:1|global");

  // The later claim being active too is not supersession.
  fixture other;
  other.task(1, "doing");
  other.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  other.claim(2, 1, "active", "2026-06-01T10:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  CHECK(other.run({"claim-superseded-active"}).findings.empty());

  // An earlier ended claim behind a later active one is history, not a finding.
  fixture history;
  history.task(1, "doing");
  history.claim(1, 1, "aborted", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z",
                "2026-06-01T09:05:00.000Z");
  history.claim(2, 1, "active", "2026-06-01T10:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  CHECK(history.run({"claim-superseded-active"}).findings.empty());
}

TEST_CASE("claim-superseded-active keeps one fingerprint when the later claim appears, and a new evidence digest",
          "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "done");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  auto before = fx.run({"claim-superseded-active"});
  REQUIRE(before.findings.size() == 1);
  CHECK(before.findings[0].evidence.size() == 2);

  // The recovery claim B is taken and ends afterwards.
  fx.claim(2, 1, "completed", "2026-06-01T10:00:00.000Z", "2026-06-01T10:00:00.000Z", "2026-06-01T10:10:00.000Z",
           "2026-06-01T10:05:00.000Z");
  auto after = fx.run({"claim-superseded-active"});
  REQUIRE(after.findings.size() == 1);
  CHECK(after.findings[0].evidence.size() == 3);

  CHECK(im::finding_fingerprint(before.findings[0]) == im::finding_fingerprint(after.findings[0]));
  CHECK(im::finding_digest(before.findings[0]) != im::finding_digest(after.findings[0]));
}

TEST_CASE("claim-superseded-active does not treat a later shared claim as supersession", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.entity_claim(1, "task", 1, "shared", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                  "2026-06-01T12:05:00.000Z");
  fx.entity_claim(2, "task", 1, "shared", "completed", "2026-06-01T10:00:00.000Z", "2026-06-01T10:00:00.000Z",
                  "2026-06-01T10:10:00.000Z", "2026-06-01T10:05:00.000Z");
  CHECK(fx.run({"claim-superseded-active"}).findings.empty());

  // One side shared is not supersession either.
  fixture mixed;
  mixed.task(1, "doing");
  mixed.entity_claim(1, "task", 1, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                     "2026-06-01T12:05:00.000Z");
  mixed.entity_claim(2, "task", 1, "shared", "completed", "2026-06-01T10:00:00.000Z", "2026-06-01T10:00:00.000Z",
                     "2026-06-01T10:10:00.000Z", "2026-06-01T10:05:00.000Z");
  CHECK(mixed.run({"claim-superseded-active"}).findings.empty());
}

TEST_CASE("claims on plans and plan steps follow their own entity status and plan scope", "[engine][diagnose][claims]") {
  fixture fx;
  fx.set_plan_status(1, "done");
  fx.set_plan_status(2, "active");
  fx.step(10, 1, "done");
  fx.step(11, 1, "pending");
  fx.step(20, 2, "skipped");
  // A plan claim on a done plan, one on an active plan, a step claim on a done step, a pending one,
  // and a step claim in the other plan on a skipped step.
  fx.entity_claim(1, "plan", 1, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                  "2026-06-01T12:05:00.000Z");
  fx.entity_claim(2, "plan", 2, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                  "2026-06-01T12:05:00.000Z");
  fx.entity_claim(3, "plan_step", 10, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                  "2026-06-01T12:05:00.000Z");
  fx.entity_claim(4, "plan_step", 11, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                  "2026-06-01T12:05:00.000Z");
  fx.entity_claim(5, "plan_step", 20, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z",
                  "2026-06-01T12:05:00.000Z");

  auto all = fx.run({"claim-superseded-active"});
  CHECK(ids_of(all) == std::vector<std::string>{"claim-superseded-active claim:1", "claim-superseded-active claim:3",
                                                "claim-superseded-active claim:5"});
  REQUIRE(all.findings.size() == 3);
  CHECK(std::ranges::contains(all.findings[0].evidence, im::entity_ref{.kind = "plan", .id = 1}));
  CHECK(std::ranges::contains(all.findings[1].evidence, im::entity_ref{.kind = "plan_step", .id = 10}));

  // The plan scope reaches a step claim through its step's plan.
  auto scoped = fx.run({"claim-superseded-active"}, k_now, 1);
  CHECK(ids_of(scoped) == std::vector<std::string>{"claim-superseded-active claim:1", "claim-superseded-active claim:3"});
  auto other = fx.run({"claim-superseded-active"}, k_now, 2);
  CHECK(ids_of(other) == std::vector<std::string>{"claim-superseded-active claim:5"});
}

TEST_CASE("claim-process-died scopes plan and plan-step claims through their plan", "[engine][diagnose][claims]") {
  fixture fx;
  fx.step(10, 1, "pending");
  fx.step(20, 2, "pending");
  fx.entity_claim(1, "plan", 1, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z",
                  "2026-06-01T09:10:00.000Z");
  fx.entity_claim(2, "plan", 2, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z",
                  "2026-06-01T09:10:00.000Z");
  fx.entity_claim(3, "plan_step", 10, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z",
                  "2026-06-01T09:10:00.000Z");
  fx.entity_claim(4, "plan_step", 20, "exclusive", "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z",
                  "2026-06-01T09:10:00.000Z");
  CHECK(ids_of(fx.run({"claim-process-died"}, k_now, 1)) ==
        std::vector<std::string>{"claim-process-died claim:1", "claim-process-died claim:3"});
  CHECK(fx.run({"claim-process-died"}).findings.size() == 4);
}

TEST_CASE("task-doing-unclaimed reports a doing task with no active unexpired claim", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing"); // no claim at all
  fx.task(2, "doing"); // only an expired active claim
  fx.task(3, "doing"); // only a stale claim
  fx.task(4, "doing"); // a live claim
  fx.task(5, "todo");  // not doing
  fx.task(6, "doing"); // a stale claim whose lease timestamp is still in the future
  fx.claim(2, 2, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T09:40:00.000Z");
  fx.claim(3, 3, "stale", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z",
           "2026-06-01T10:00:00.000Z");
  fx.claim(4, 4, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  fx.claim(6, 6, "stale", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z",
           "2026-06-01T11:56:00.000Z");

  auto d = fx.run({"task-doing-unclaimed"});
  CHECK(ids_of(d) == std::vector<std::string>{"task-doing-unclaimed task:1", "task-doing-unclaimed task:2",
                                              "task-doing-unclaimed task:3", "task-doing-unclaimed task:6"});
  REQUIRE(d.findings.size() == 4);
  for (const auto& f : d.findings) {
    CHECK(f.severity == im::diagnostic_severity::warning);
    CHECK(f.recovery.contains("--no-transition"));
  }
  // The evidence time is the latest claim's release time, else its lease expiry, else (no claim ever)
  // the task's own updated_at.
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{"2026-05-02T00:00:00.000Z"});
  CHECK(d.findings[1].evidence_times == std::vector<std::string>{"2026-06-01T09:40:00.000Z"});
  CHECK(d.findings[2].evidence_times == std::vector<std::string>{"2026-06-01T10:00:00.000Z"});
  CHECK(d.findings[3].evidence_times == std::vector<std::string>{"2026-06-01T11:56:00.000Z"});
}

TEST_CASE("task-doing-unclaimed keeps its digest when the task is edited", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "stale", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z",
           "2026-06-01T10:00:00.000Z");
  auto before = fx.run({"task-doing-unclaimed"});
  exec(fx.conn, "update tasks set updated_at = '2026-06-01T11:00:00.000Z', title = 'edited' where id = 1");
  auto after = fx.run({"task-doing-unclaimed"});
  REQUIRE(before.findings.size() == 1);
  REQUIRE(after.findings.size() == 1);
  CHECK(im::finding_digest(before.findings[0]) == im::finding_digest(after.findings[0]));
}

TEST_CASE("claim-closed-by-reconcile reports a stale claim inside the window as info", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "todo");
  fx.claim(1, 1, "stale", "2026-05-31T09:00:00.000Z", "2026-05-31T09:00:00.000Z", "2026-05-31T09:10:00.000Z",
           "2026-05-31T10:00:00.000Z");
  fx.claim(2, 1, "stale", "2026-04-01T09:00:00.000Z", "2026-04-01T09:00:00.000Z", "2026-04-01T09:10:00.000Z",
           "2026-04-01T10:00:00.000Z");
  fx.claim(3, 1, "completed", "2026-05-31T09:00:00.000Z", "2026-05-31T09:00:00.000Z", "2026-05-31T09:10:00.000Z",
           "2026-05-31T09:05:00.000Z");

  auto d = fx.run({"claim-closed-by-reconcile"});
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::info);
  CHECK(im::entity_ref_text(d.findings[0].primary) == "claim:1");
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{"2026-05-31T10:00:00.000Z"});
  CHECK(d.findings[0].recovery.empty());
  // A wider window brings the older one in.
  CHECK(fx.run({"claim-closed-by-reconcile"}, k_now, std::nullopt, 90).findings.size() == 2);
}

TEST_CASE("a healthy claim, a finished claim and a todo task give no claim finding", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.task(2, "done");
  fx.task(3, "todo");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T11:55:00.000Z", "2026-06-01T12:05:00.000Z");
  fx.heartbeat_action(1, 1, "2026-06-01T11:55:00.000Z");
  fx.claim(2, 2, "completed", "2026-06-01T08:00:00.000Z", "2026-06-01T08:30:00.000Z", "2026-06-01T08:40:00.000Z",
           "2026-06-01T08:35:00.000Z");
  fx.heartbeat_action(2, 2, "2026-06-01T08:30:00.000Z");
  fx.claim(3, 3, "released", "2026-06-01T07:00:00.000Z", "2026-06-01T07:00:00.000Z", "2026-06-01T07:10:00.000Z",
           "2026-06-01T07:05:00.000Z");

  auto d = fx.run(k_claim_checks);
  INFO(dg::render_text(d));
  CHECK(d.findings.empty());
  CHECK(d.result == dg::run_outcome::ok);
  for (const auto& c : d.checks) {
    CHECK((c.state == dg::check_state::ran) == std::ranges::contains(k_claim_checks, c.id));
  }
}

TEST_CASE("the plan scope limits claim findings to the plan's tasks", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing", 1);
  fx.task(2, "doing", 2);
  auto d = fx.run({"task-doing-unclaimed"}, k_now, 1);
  CHECK(ids_of(d) == std::vector<std::string>{"task-doing-unclaimed task:1"});
  CHECK(fx.run({"task-doing-unclaimed"}).findings.size() == 2);
}

TEST_CASE("evidence times are row timestamps and do not move with the evaluation instant", "[engine][diagnose][claims]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:00:00.000Z", "2026-06-01T09:10:00.000Z");
  auto first  = fx.run({"claim-process-died", "task-doing-unclaimed"}, "2026-06-01T12:00:00.000Z");
  auto second = fx.run({"claim-process-died", "task-doing-unclaimed"}, "2026-06-02T12:00:00.000Z");
  REQUIRE(first.findings.size() == 2);
  REQUIRE(second.findings.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(first.findings[i].evidence_times == second.findings[i].evidence_times);
    CHECK(im::finding_digest(first.findings[i]) == im::finding_digest(second.findings[i]));
  }
}

namespace {

using namespace std::chrono_literals;

/// A timestamp `offset` after 09:00:00.000 on the fixture day, in the stored format.
auto at_offset(std::chrono::milliseconds offset) -> std::string {
  auto base = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::sys_days{std::chrono::year{2026} / 6 / 1}} + 9h;
  return std::format("{:%FT%T}Z", base + offset);
}

/// A claim with a 600 s lease, claimed at `at_offset(0)`, with one heartbeat action `gap` later.
auto gap_fixture(fixture& fx, std::chrono::milliseconds gap) -> void {
  fx.task(1, "doing");
  fx.claim(1, 1, "completed", at_offset(0ms), at_offset(gap), at_offset(gap + 600s), at_offset(gap + 1s));
  fx.heartbeat_action(1, 1, at_offset(gap));
}

auto gap_findings(fixture& fx) -> dg::diagnosis {
  return fx.run({"heartbeat-gap"}, "2026-06-02T00:00:00.000Z");
}

} // namespace

TEST_CASE("heartbeat-gap is catalogued as a warning event", "[engine][diagnose][claims][heartbeat-gap]") {
  auto cat = dg::builtin_catalog();
  auto it  = std::ranges::find(cat.checks, "heartbeat-gap", &dg::check_def::id);
  REQUIRE(it != cat.checks.end());
  CHECK(it->built);
  CHECK(it->kind == im::check_kind::event);
  CHECK(it->severity == im::diagnostic_severity::warning);
  CHECK(it->category == "heartbeat_gap");
  CHECK(it->recovery.contains("half"));
}

TEST_CASE("heartbeat-gap is silent at exactly half the lease and reports info just beyond it",
          "[engine][diagnose][claims][heartbeat-gap]") {
  fixture half;
  gap_fixture(half, 300s);
  CHECK(gap_findings(half).findings.empty());

  fixture beyond;
  gap_fixture(beyond, 300s + 1ms);
  auto d = gap_findings(beyond);
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::info);
  CHECK(d.findings[0].check_id == "heartbeat-gap");
  CHECK(im::entity_ref_text(d.findings[0].primary) == "claim:1");
  CHECK(std::ranges::contains(d.findings[0].evidence, im::entity_ref{.kind = "task", .id = 1}));
}

TEST_CASE("a gap between recorded heartbeats is never more than info, however long",
          "[engine][diagnose][claims][heartbeat-gap]") {
  for (std::chrono::milliseconds gap :
       {std::chrono::milliseconds{600s}, 600s + 1ms, std::chrono::milliseconds{900s}, std::chrono::milliseconds{5000s}}) {
    fixture fx;
    gap_fixture(fx, gap);
    auto d = gap_findings(fx);
    INFO(gap.count());
    REQUIRE(d.findings.size() == 1);
    CHECK(d.findings[0].severity == im::diagnostic_severity::info);
  }
}

namespace {

/// A claim with a 600 s lease and one heartbeat at +100 s, which is also its `last_heartbeat_at`.
/// It ends `trailing` after that heartbeat: released then (a terminal claim), or still active and
/// evaluated then.
auto trailing_fixture(fixture& fx, std::chrono::milliseconds trailing, bool terminal) -> std::string {
  fx.task(1, "doing");
  if (terminal) {
    fx.claim(1, 1, "completed", at_offset(0ms), at_offset(100s), at_offset(700s), at_offset(100s + trailing));
  } else {
    fx.claim(1, 1, "active", at_offset(0ms), at_offset(100s), at_offset(700s));
  }
  fx.heartbeat_action(1, 1, at_offset(100s));
  return at_offset(100s + trailing);
}

} // namespace

TEST_CASE("heartbeat-gap warns for a terminal claim only when the trailing stretch is beyond the lease",
          "[engine][diagnose][claims][heartbeat-gap]") {
  fixture at;
  auto    end = trailing_fixture(at, 600s, true);
  CHECK(at.run({"heartbeat-gap"}, "2026-06-02T00:00:00.000Z").findings.empty());

  fixture beyond;
  end    = trailing_fixture(beyond, 600s + 1ms, true);
  auto d = beyond.run({"heartbeat-gap"}, "2026-06-02T00:00:00.000Z");
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::warning);
  CHECK(im::entity_ref_text(d.findings[0].primary) == "claim:1");
  // The stretch runs from the last heartbeat to the release.
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{at_offset(100s), end});
}

TEST_CASE("heartbeat-gap warns for an active claim when the stretch to now is beyond the lease",
          "[engine][diagnose][claims][heartbeat-gap]") {
  fixture at;
  auto    end = trailing_fixture(at, 600s, false);
  CHECK(at.run({"heartbeat-gap"}, end).findings.empty());

  fixture beyond;
  trailing_fixture(beyond, 600s, false);
  auto d = beyond.run({"heartbeat-gap"}, at_offset(100s + 600s + 1ms));
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::warning);
  // The evidence is row timestamps: the last heartbeat and the lease expiry, not the instant.
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{at_offset(100s), at_offset(700s)});
  auto later = beyond.run({"heartbeat-gap"}, at_offset(100s + 900s));
  REQUIRE(later.findings.size() == 1);
  CHECK(im::finding_digest(later.findings[0]) == im::finding_digest(d.findings[0]));
}

TEST_CASE("heartbeat-gap measures the trailing stretch from claiming when there was never a heartbeat",
          "[engine][diagnose][claims][heartbeat-gap]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", at_offset(0ms), at_offset(0ms), at_offset(600s));
  CHECK(fx.run({"heartbeat-gap"}, at_offset(600s)).findings.empty());
  auto d = fx.run({"heartbeat-gap"}, at_offset(600s + 1ms));
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::warning);
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{at_offset(0ms), at_offset(600s)});
}

TEST_CASE("heartbeat-gap counts last_heartbeat_at as a heartbeat point", "[engine][diagnose][claims][heartbeat-gap]") {
  // Plain heartbeats write no action row, only last_heartbeat_at: 500 s after claiming on a 600 s
  // lease is beyond half (info), and the stretch after it is short.
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "completed", at_offset(0ms), at_offset(500s), at_offset(1100s), at_offset(550s));
  auto d = gap_findings(fx);
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::info);
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{at_offset(0ms), at_offset(500s)});
}

TEST_CASE("heartbeat-gap measures between consecutive heartbeats and derives the lease from the claim row",
          "[engine][diagnose][claims][heartbeat-gap]") {
  fixture fx;
  fx.task(1, "doing");
  // A 1200 s lease: a 700 s gap is beyond half but not beyond the full lease.
  fx.claim(1, 1, "completed", at_offset(0ms), at_offset(1500s), at_offset(1500s + 1200s), at_offset(1600s));
  fx.heartbeat_action(1, 1, at_offset(100s));  // 100 s after claiming
  fx.heartbeat_action(2, 1, at_offset(800s));  // 700 s gap: info
  fx.heartbeat_action(3, 1, at_offset(1000s)); // 200 s gap: nothing
  fx.heartbeat_action(4, 1, at_offset(1500s)); // 500 s gap: nothing
  auto d = gap_findings(fx);
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::info);
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{at_offset(100s), at_offset(800s)});
  CHECK(d.findings[0].recovery.contains("half"));
}

TEST_CASE("heartbeat-gap counts the stretch from claiming to the first heartbeat", "[engine][diagnose][claims][heartbeat-gap]") {
  fixture fx;
  gap_fixture(fx, 900s);
  auto d = gap_findings(fx);
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].severity == im::diagnostic_severity::info);
  CHECK(d.findings[0].evidence_times == std::vector<std::string>{at_offset(0ms), at_offset(900s)});
}

TEST_CASE("heartbeat-gap keeps one finding per gap and lets a steady cadence pass", "[engine][diagnose][claims][heartbeat-gap]") {
  fixture steady;
  steady.task(1, "doing");
  steady.claim(1, 1, "completed", at_offset(0ms), at_offset(1200s), at_offset(1800s), at_offset(1300s));
  for (int i = 1; i <= 4; ++i) {
    steady.heartbeat_action(i, 1, at_offset(std::chrono::seconds{300 * i}));
  }
  CHECK(gap_findings(steady).findings.empty());

  fixture gappy;
  gappy.task(1, "doing");
  gappy.claim(1, 1, "completed", at_offset(0ms), at_offset(2400s), at_offset(3000s), at_offset(2500s));
  gappy.heartbeat_action(1, 1, at_offset(700s));
  gappy.heartbeat_action(2, 1, at_offset(1500s));
  gappy.heartbeat_action(3, 1, at_offset(2400s));
  CHECK(gap_findings(gappy).findings.size() == 3);
}

TEST_CASE("heartbeat-gap is bounded by the window and the plan scope", "[engine][diagnose][claims][heartbeat-gap]") {
  fixture fx;
  fx.task(1, "doing", 1);
  fx.task(2, "doing", 2);
  fx.claim(1, 1, "completed", at_offset(0ms), at_offset(900s), at_offset(1500s), at_offset(1000s));
  fx.heartbeat_action(1, 1, at_offset(900s));
  fx.claim(2, 2, "completed", at_offset(0ms), at_offset(900s), at_offset(1500s), at_offset(1000s));
  fx.heartbeat_action(2, 2, at_offset(900s));
  // A second heartbeat row gives the out-of-scope claim a gap between heartbeat rows alone.
  fx.heartbeat_action(3, 2, at_offset(1800s));

  CHECK(fx.run({"heartbeat-gap"}, "2026-06-02T00:00:00.000Z").findings.size() == 3);
  auto scoped = fx.run({"heartbeat-gap"}, "2026-06-02T00:00:00.000Z", 1);
  REQUIRE(scoped.findings.size() == 1);
  CHECK(im::entity_ref_text(scoped.findings[0].primary) == "claim:1");
  // The gap ended 2026-06-01T09:15; a window that starts after it excludes it.
  CHECK(fx.run({"heartbeat-gap"}, "2026-06-09T00:00:00.000Z", std::nullopt, 3).findings.empty());
}

TEST_CASE("heartbeat-gap skips a claim whose lease length is not positive", "[engine][diagnose][claims][heartbeat-gap]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "completed", at_offset(0ms), at_offset(900s), at_offset(900s), at_offset(1000s));
  fx.heartbeat_action(1, 1, at_offset(900s));
  CHECK(gap_findings(fx).findings.empty());
}

TEST_CASE("heartbeat-gap evidence does not move with the evaluation instant", "[engine][diagnose][claims][heartbeat-gap]") {
  fixture fx;
  gap_fixture(fx, 900s);
  auto first  = fx.run({"heartbeat-gap"}, "2026-06-02T00:00:00.000Z");
  auto second = fx.run({"heartbeat-gap"}, "2026-06-03T00:00:00.000Z");
  REQUIRE(first.findings.size() == 1);
  REQUIRE(second.findings.size() == 1);
  CHECK(im::finding_digest(first.findings[0]) == im::finding_digest(second.findings[0]));
}
