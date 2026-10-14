// @file checks_dispatch.t.cpp
// @brief Tests for the dispatch-family check of `planar.engine.diagnose` (plan 1132).
//
// These cases drive the shipped catalog over a scratch database with rows written directly, so
// the evaluation instant and every boundary are exact. The same check runs through the real
// `planar-agent` verbs in `src/cmd/planar-watch/diagnose_checks.t.cpp`.
//
// What is pinned:
//
//   * `action-unended` (state, warning): an action with no `ended_at` whose claim is terminal or
//     past its lease.
//   * `dispatch-no-role-action` and `dispatch-confirmed-late` are gone from the catalog: they read
//     the routing dispatch tables migration 00043 dropped.

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
      : path_(std::filesystem::temp_directory_path() / std::format("planar_diagnose_dispatch_{}_{}.db",
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

auto quoted_or_null(std::string_view text) -> std::string {
  return text.empty() ? std::string{"null"} : std::format("'{}'", text);
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

  /// One exclusive claim row on a task, with token `tok<id>`. `released` is empty for an unended claim.
  auto claim(int id, int task_id, std::string_view status, std::string_view claimed, std::string_view last_heartbeat,
             std::string_view lease_expires, std::string_view released = {}) -> void {
    exec(conn, std::format("insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, "
                           "status, vendor, role, claimed_at, last_heartbeat_at, lease_expires_at, released_at) values ({}, "
                           "'tok{}', 1, 'task', {}, 'exclusive', '{}', 'test', 'coder', '{}', '{}', '{}', {})",
                           id, id, task_id, status, claimed, last_heartbeat, lease_expires, quoted_or_null(released)));
  }

  /// A claim that is active with its lease still running at the evaluation instant.
  auto live_claim(int id, int task_id) -> void {
    claim(id, task_id, "active", "2026-06-01T11:00:00.000Z", "2026-06-01T11:50:00.000Z", "2026-06-01T12:20:00.000Z");
  }

  /// A claim that is `active` but past its lease at the evaluation instant.
  auto lapsed_claim(int id, int task_id) -> void {
    claim(id, task_id, "active", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T09:40:00.000Z");
  }

  /// One action. `claim_id` 0 leaves it untied to a claim, `task_id` 0 leaves it untied to a task, `ended` empty leaves it open.
  auto action(int id, int claim_id, std::string_view kind, int task_id, std::string_view started, std::string_view ended = {})
      -> void {
    exec(conn, std::format("insert into agent_actions (id, session_id, claim_id, action_kind, entity_kind, entity_id, vendor, "
                           "started_at, ended_at, outcome) values ({}, 1, {}, '{}', {}, {}, 'test', '{}', {}, {})",
                           id, claim_id == 0 ? std::string{"null"} : std::to_string(claim_id), kind,
                           task_id == 0 ? std::string{"null"} : std::string{"'task'"},
                           task_id == 0 ? std::string{"null"} : std::to_string(task_id), started, quoted_or_null(ended),
                           ended.empty() ? std::string{"null"} : std::string{"'ok'"}));
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

} // namespace

TEST_CASE("action-unended is catalogued with the spec's kind, severity and category; the dispatch checks are retired",
          "[engine][diagnose][dispatch]") {
  auto cat = dg::builtin_catalog();
  auto it  = std::ranges::find(cat.checks, std::string_view{"action-unended"}, &dg::check_def::id);
  REQUIRE(it != cat.checks.end());
  CHECK(it->built);
  CHECK(it->kind == im::check_kind::state);
  CHECK(it->severity == im::diagnostic_severity::warning);
  CHECK(it->category == "claim_action_unended");
  CHECK(it->inputs.empty());
  for (std::string_view retired : {"dispatch-no-role-action", "dispatch-confirmed-late"}) {
    INFO(retired);
    CHECK(std::ranges::find(cat.checks, retired, &dg::check_def::id) == cat.checks.end());
  }
}

TEST_CASE("action-unended reports an open action whose claim lapsed", "[engine][diagnose][actions]") {
  fixture fx;
  fx.task(1, "doing");
  fx.lapsed_claim(1, 1); // past its lease at 09:40
  fx.action(1, 1, "claim_check", 1, "2026-06-01T09:00:00.000Z");
  fx.action(2, 1, "tool_call", 1, "2026-06-01T09:10:00.000Z");
  fx.action(3, 1, "coder", 1, "2026-06-01T09:05:00.000Z", "2026-06-01T09:20:00.000Z"); // ended: not reported

  auto d = fx.run({"action-unended"});
  REQUIRE(d.findings.size() == 2);
  CHECK(ids_of(d) == std::vector<std::string>{"action-unended action:1", "action-unended action:2"});
  const auto& f = d.findings[1];
  CHECK(f.severity == im::diagnostic_severity::warning);
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "claim", .id = 1}));
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "task", .id = 1}));
  CHECK(f.evidence_times == std::vector<std::string>{"2026-06-01T09:10:00.000Z"});
  // `reconcile` does not end an action, but `action end` does, whatever state its claim is in (decision 1384).
  CHECK(f.recovery.contains("planar-agent action end --action <id>"));
  CHECK(!f.recovery.contains("reconcile"));
}

TEST_CASE("action-unended is silent while the lease runs and starts when it lapses", "[engine][diagnose][actions]") {
  fixture fx;
  fx.task(1, "doing");
  fx.claim(1, 1, "active", "2026-06-01T11:00:00.000Z", "2026-06-01T11:50:00.000Z", std::string{k_now});
  fx.action(1, 1, "tool_call", 1, "2026-06-01T11:10:00.000Z");
  // A lease that expires exactly now is live.
  CHECK(fx.run({"action-unended"}).findings.empty());
  CHECK(ids_of(fx.run({"action-unended"}, "2026-06-01T12:00:00.001Z")) == std::vector<std::string>{"action-unended action:1"});
  // A claim well inside its lease is live.
  fixture healthy;
  healthy.task(1, "doing");
  healthy.live_claim(1, 1);
  healthy.action(1, 1, "tool_call", 1, "2026-06-01T11:10:00.000Z");
  CHECK(healthy.run({"action-unended"}).findings.empty());
}

TEST_CASE("action-unended reports an open action on a claim that ended through any terminal status",
          "[engine][diagnose][actions]") {
  fixture fx;
  fx.task(1, "done");
  fx.task(2, "done");
  fx.task(3, "done");
  fx.claim(1, 1, "completed", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T09:40:00.000Z",
           "2026-06-01T10:00:00.000Z");
  fx.claim(2, 2, "stale", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T11:59:00.000Z",
           "2026-06-01T10:00:00.000Z");
  fx.claim(3, 3, "aborted", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T11:59:00.000Z",
           "2026-06-01T10:00:00.000Z");
  fx.action(1, 1, "tool_call", 1, "2026-06-01T09:10:00.000Z");
  fx.action(2, 2, "coder", 2, "2026-06-01T09:10:00.000Z");
  fx.action(3, 3, "reviewer", 3, "2026-06-01T09:10:00.000Z");
  // An open action with no claim has nothing to be unended against.
  fx.action(4, 0, "tool_call", 3, "2026-06-01T09:10:00.000Z");
  CHECK(ids_of(fx.run({"action-unended"})) ==
        std::vector<std::string>{"action-unended action:1", "action-unended action:2", "action-unended action:3"});
}

TEST_CASE("action-unended reports only actions started, or claims ended, inside the window; the plan scope narrows it",
          "[engine][diagnose][actions][calibration]") {
  // Decision 1384: 318 of 363 findings on the operator's database predated the 30-day window, so the window
  // now bounds this check, by the action's start or by the end of its claim (its release, else its lease expiry).
  fixture fx;
  fx.task(1, "doing", 1);
  fx.task(2, "doing", 2);
  fx.task(3, "done", 1);
  fx.task(4, "done", 1);
  // Window default: 7 days ending 2026-06-01T12:00, so it starts 2026-05-25T12:00.
  fx.claim(1, 1, "active", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z");
  fx.claim(2, 2, "active", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z");
  fx.action(1, 1, "tool_call", 1, "2026-04-01T09:10:00.000Z"); // old action, claim lapsed in April: outside
  fx.action(2, 2, "tool_call", 2, "2026-04-01T09:10:00.000Z"); // the same on another plan: outside
  CHECK(fx.run({"action-unended"}).findings.empty());
  CHECK(ids_of(fx.run({"action-unended"}, k_now, std::nullopt, 90)) ==
        std::vector<std::string>{"action-unended action:1", "action-unended action:2"});
  CHECK(ids_of(fx.run({"action-unended"}, k_now, 2, 90)) == std::vector<std::string>{"action-unended action:2"});

  // The claim ended inside the window although the action started long before it.
  fx.claim(3, 3, "stale", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z",
           "2026-05-30T09:00:00.000Z");
  fx.action(3, 3, "coder", 3, "2026-04-01T09:10:00.000Z");
  // The action started inside the window although its claim was taken long before it.
  fx.claim(4, 4, "aborted", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z",
           "2026-04-02T09:00:00.000Z");
  fx.action(4, 4, "coder", 4, "2026-05-28T09:10:00.000Z");
  CHECK(ids_of(fx.run({"action-unended"})) == std::vector<std::string>{"action-unended action:3", "action-unended action:4"});

  // The window edges are inclusive, on the claim's end and on the action's start, compared as instants.
  fixture edge;
  edge.task(1, "done");
  edge.task(2, "done");
  edge.claim(1, 1, "stale", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z",
             "2026-05-25T12:00:00.000Z");
  edge.action(1, 1, "coder", 1, "2026-04-01T09:10:00.000Z");
  edge.claim(2, 2, "stale", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z",
             "2026-05-25T11:59:59.999Z");
  edge.action(2, 2, "coder", 2, "2026-04-01T09:10:00.000Z");
  CHECK(ids_of(edge.run({"action-unended"})) == std::vector<std::string>{"action-unended action:1"});
}

TEST_CASE("action-unended evidence does not move with the evaluation instant", "[engine][diagnose][actions]") {
  fixture fx;
  fx.task(1, "doing");
  fx.lapsed_claim(1, 1);
  fx.action(1, 1, "tool_call", 1, "2026-06-01T09:10:00.000Z");
  auto first = fx.run({"action-unended"});
  auto later = fx.run({"action-unended"}, "2026-06-02T12:00:00.000Z");
  REQUIRE(first.findings.size() == 1);
  REQUIRE(later.findings.size() == 1);
  CHECK(im::finding_fingerprint(first.findings[0]) == im::finding_fingerprint(later.findings[0]));
  CHECK(im::finding_digest(first.findings[0]) == im::finding_digest(later.findings[0]));
}
