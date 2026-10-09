// @file checks_dispatch.t.cpp
// @brief Tests for the dispatch-family checks of `planar.engine.diagnose` (plan 1132, tasks 7376 and 7377).
//
// These cases drive the shipped catalog over a scratch database with rows written directly, so
// the evaluation instant and every boundary are exact. The same checks run through the real
// `planar-agent` verbs in `src/cmd/planar-watch/diagnose_checks.t.cpp`.
//
// What is pinned (decisions 1344 and 1349: a claim is a dispatch only when a dispatch preview
// with its task and claim token, or a dispatch snapshot for its task overlapping the claim,
// exists; a direct claim is never considered):
//
//   * `dispatch-no-role-action` (event, error): a dispatch whose claim is no longer live and that
//     has no `coder`, `reviewer` or `test_coder` action tied to the claim, or to its task inside
//     the claim's lifetime.
//   * `action-unended` (state, warning): an action with no `ended_at` whose claim is terminal or
//     past its lease.
//   * A direct claim, a live claim, and a preview for another task or token produce no dispatch
//     finding.

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
    // The two rows a dispatch preview and snapshot reference; no verb creates them.
    exec(conn, "insert into projects (id, slug, name) values (1, 'routing-project', 'Routing Project')");
    exec(conn, "insert into routing_candidates (id, vendor, candidate_id, fallback_order) values (1, 'test', 'candidate', 0)");
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

  /// A claim that was taken at 09:00, ran and was completed at 10:00: not live.
  auto completed_claim(int id, int task_id) -> void {
    claim(id, task_id, "completed", "2026-06-01T09:00:00.000Z", "2026-06-01T09:30:00.000Z", "2026-06-01T09:40:00.000Z",
          "2026-06-01T10:00:00.000Z");
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

  /// One dispatch snapshot for a task, confirmed at `confirmed`.
  auto snapshot(int id, int task_id, std::string_view confirmed) -> void {
    exec(conn, std::format("insert into routing_dispatch_snapshots (id, dispatch_key, task_id, logical_work_item_id, project_id, "
                           "validation_policy_version, routing_policy_version, profile_rule_version, vendor, role, tier, "
                           "work_type, complexity, packet_digest, policy_digest, capability_digest, requested_candidate_id, "
                           "assignment_class, operator_decision, reviewer_disposition, confirmed_at) values ({}, 'key{}', {}, "
                           "'w{}', 1, 'v1', 'r1', 'p1', 'test', 'coder', 'medium', 'feature', 'standard', 'pk', 'po', 'cp', 1, "
                           "'default', 'confirmed', 'required', '{}')",
                           id, id, task_id, id, confirmed));
  }

  /// One dispatch preview for a task, bound to `token` (empty for none), created at `created`. A
  /// non-zero `consumed_by` marks it spent by that snapshot at `consumed_at`.
  auto preview(int id, int task_id, std::string_view token, std::string_view created, int consumed_by = 0,
               std::string_view consumed_at = {}) -> void {
    exec(conn, std::format("insert into routing_dispatch_previews (id, preview_token, task_id, logical_work_item_id, project_id, "
                           "validation_policy_version, routing_policy_version, profile_rule_version, vendor, role, tier, "
                           "work_type, complexity, packet_digest, profile_digest, policy_digest, capability_digest, "
                           "requested_candidate_id, host_id, assignment_class, claim_token, evidence_state, created_at, "
                           "expires_at, consumed_at, consumed_dispatch_id) values ({}, 'ptok{}', {}, 'w{}', 1, 'v1', 'r1', 'p1', "
                           "'test', 'coder', 'medium', 'feature', 'standard', 'pk', 'pf', 'po', 'cp', 1, 'host', 'default', {}, "
                           "'evidential', '{}', '2099-01-01T00:00:00.000Z', {}, {})",
                           id, id, task_id, id, quoted_or_null(token), created, quoted_or_null(consumed_at),
                           consumed_by == 0 ? std::string{"null"} : std::to_string(consumed_by)));
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

const std::vector<std::string> k_no_role{"dispatch-no-role-action"};

} // namespace

TEST_CASE("the dispatch and action checks are catalogued with the spec's kind, severity and category",
          "[engine][diagnose][dispatch]") {
  auto cat = dg::builtin_catalog();
  struct row {
    std::string_view        id;
    im::check_kind          kind;
    im::diagnostic_severity severity;
    std::string_view        category;
  };
  for (const auto& want : std::vector<row>{
           {"dispatch-no-role-action", im::check_kind::event, im::diagnostic_severity::error, "dispatch_no_role_action"},
           {"action-unended", im::check_kind::state, im::diagnostic_severity::warning, "claim_action_unended"}}) {
    auto it = std::ranges::find(cat.checks, want.id, &dg::check_def::id);
    INFO(want.id);
    REQUIRE(it != cat.checks.end());
    CHECK(it->built);
    CHECK(it->kind == want.kind);
    CHECK(it->severity == want.severity);
    CHECK(it->category == want.category);
    CHECK(it->inputs.empty());
  }
}

TEST_CASE("dispatch-no-role-action reports a previewed dispatch whose claim ended with no role action",
          "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done");
  fx.completed_claim(1, 1);
  fx.snapshot(1, 1, "2026-06-01T09:05:00.000Z");
  fx.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z", 1, "2026-06-01T09:05:00.000Z");
  // Work the claim did that is not a role action does not count.
  fx.action(1, 1, "claim_check", 1, "2026-06-01T09:00:00.000Z", "2026-06-01T10:00:00.000Z");
  fx.action(2, 1, "tool_call", 1, "2026-06-01T09:10:00.000Z", "2026-06-01T09:11:00.000Z");

  auto d = fx.run(k_no_role);
  REQUIRE(d.findings.size() == 1);
  const auto& f = d.findings[0];
  CHECK(f.check_id == "dispatch-no-role-action");
  CHECK(f.severity == im::diagnostic_severity::error);
  CHECK(im::entity_ref_text(f.primary) == "claim:1");
  // The evidence names the claim, its task, the preview and the snapshot.
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "claim", .id = 1}));
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "task", .id = 1}));
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "dispatch_preview", .id = 1}));
  CHECK(std::ranges::contains(f.evidence, im::entity_ref{.kind = "dispatch_snapshot", .id = 1}));
  CHECK(f.evidence.size() == 4);
  // A row timestamp: the claim's release.
  CHECK(f.evidence_times == std::vector<std::string>{"2026-06-01T10:00:00.000Z"});
  CHECK(f.recovery.contains("task-tied action"));
}

TEST_CASE("a direct claim is never a dispatch, whether or not it started an action", "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done");
  fx.task(2, "done");
  fx.completed_claim(1, 1); // no record of any kind, no role action
  fx.completed_claim(2, 2);
  fx.action(1, 2, "coder", 2, "2026-06-01T09:10:00.000Z", "2026-06-01T09:50:00.000Z");
  fx.lapsed_claim(3, 1);
  CHECK(fx.run(k_no_role).findings.empty());
}

TEST_CASE("a preview is a dispatch record only for its own task and claim token", "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done");
  fx.task(2, "done");
  fx.completed_claim(1, 1);
  fx.completed_claim(2, 2);
  // A preview for task 1 bound to another claim's token, and one for task 2 with no token at all.
  fx.preview(1, 1, "tok2", "2026-06-01T09:01:00.000Z");
  fx.preview(2, 2, "", "2026-06-01T09:01:00.000Z");
  // The right token on the wrong task.
  fx.task(3, "done");
  fx.completed_claim(3, 3);
  fx.preview(3, 1, "tok3", "2026-06-01T09:01:00.000Z");
  CHECK(fx.run(k_no_role).findings.empty());

  // The matching task and token is a dispatch.
  fx.preview(4, 1, "tok1", "2026-06-01T09:02:00.000Z");
  CHECK(ids_of(fx.run(k_no_role)) == std::vector<std::string>{"dispatch-no-role-action claim:1"});
}

TEST_CASE("a snapshot makes a claim a dispatch only when it overlaps the claim's lifetime", "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done");
  fx.task(2, "done");
  fx.task(3, "done");
  fx.task(4, "done");
  fx.completed_claim(1, 1); // lifetime 09:00 through 10:00
  fx.completed_claim(2, 2);
  fx.completed_claim(3, 3);
  fx.completed_claim(4, 4);
  fx.snapshot(1, 1, "2026-06-01T09:30:00.000Z"); // inside
  fx.snapshot(2, 2, "2026-06-01T08:59:59.000Z"); // before the claim
  fx.snapshot(3, 3, "2026-06-01T10:00:01.000Z"); // after its release
  fx.snapshot(4, 1, "2026-06-01T09:45:00.000Z"); // another task's: leaves claim 4 direct
  auto d = fx.run(k_no_role);
  CHECK(ids_of(d) == std::vector<std::string>{"dispatch-no-role-action claim:1"});
  // With no preview, the evidence is the claim, its task and the snapshot; the earliest overlapping one.
  REQUIRE(d.findings.size() == 1);
  CHECK(std::ranges::contains(d.findings[0].evidence, im::entity_ref{.kind = "dispatch_snapshot", .id = 1}));
  CHECK(d.findings[0].evidence.size() == 3);
}

TEST_CASE("snapshot confirmed_at is compared as an instant, not as text", "[engine][diagnose][dispatch]") {
  // `dispatch confirm --now` writes the caller's RFC 3339 text, here without a fraction. At the
  // release second it is inside the lifetime; as text '...:00Z' sorts after '...:00.000Z'.
  fixture fx;
  fx.task(1, "done");
  fx.completed_claim(1, 1); // released 10:00:00.000
  fx.snapshot(1, 1, "2026-06-01T10:00:00Z");
  CHECK(ids_of(fx.run(k_no_role)) == std::vector<std::string>{"dispatch-no-role-action claim:1"});
}

TEST_CASE("any role action tied to the claim or to its task inside the claim's lifetime satisfies the dispatch",
          "[engine][diagnose][dispatch]") {
  for (std::string_view kind : {"coder", "reviewer", "test_coder"}) {
    INFO(kind);
    fixture fx;
    fx.task(1, "done");
    fx.completed_claim(1, 1);
    fx.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
    fx.action(1, 1, kind, 1, "2026-06-01T09:10:00.000Z", "2026-06-01T09:50:00.000Z");
    CHECK(fx.run(k_no_role).findings.empty());
  }

  // A role action tied to the claim without naming a task still counts.
  fixture claim_tied;
  claim_tied.task(1, "done");
  claim_tied.completed_claim(1, 1);
  claim_tied.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  claim_tied.action(1, 1, "coder", 0, "2026-06-01T09:10:00.000Z");
  CHECK(claim_tied.run(k_no_role).findings.empty());

  // A task-tied role action under a different claim (the orchestrator's) inside the lifetime counts.
  fixture task_tied;
  task_tied.task(1, "done");
  task_tied.completed_claim(1, 1);
  task_tied.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  task_tied.action(1, 0, "coder", 1, "2026-06-01T09:10:00.000Z", "2026-06-01T09:50:00.000Z");
  CHECK(task_tied.run(k_no_role).findings.empty());

  // The same action before the claim was taken, or after it ended, is outside the lifetime.
  fixture outside;
  outside.task(1, "done");
  outside.completed_claim(1, 1);
  outside.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  outside.action(1, 0, "coder", 1, "2026-06-01T08:00:00.000Z", "2026-06-01T08:30:00.000Z");
  outside.action(2, 0, "reviewer", 1, "2026-06-01T10:00:01.000Z", "2026-06-01T10:30:00.000Z");
  CHECK(ids_of(outside.run(k_no_role)) == std::vector<std::string>{"dispatch-no-role-action claim:1"});

  // A role action on a different task does not count.
  fixture other_task;
  other_task.task(1, "done");
  other_task.task(2, "done");
  other_task.completed_claim(1, 1);
  other_task.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  other_task.action(1, 0, "coder", 2, "2026-06-01T09:10:00.000Z", "2026-06-01T09:50:00.000Z");
  CHECK(ids_of(other_task.run(k_no_role)) == std::vector<std::string>{"dispatch-no-role-action claim:1"});
}

TEST_CASE("dispatch-no-role-action waits for the claim to end", "[engine][diagnose][dispatch]") {
  // A live claim can still start its action.
  fixture live;
  live.task(1, "doing");
  live.live_claim(1, 1);
  live.preview(1, 1, "tok1", "2026-06-01T11:01:00.000Z");
  CHECK(live.run(k_no_role).findings.empty());

  // A claim whose lease expires exactly now is still live; one tick later it has lapsed.
  fixture edge;
  edge.task(1, "doing");
  edge.claim(1, 1, "active", "2026-06-01T11:00:00.000Z", "2026-06-01T11:30:00.000Z", std::string{k_now});
  edge.preview(1, 1, "tok1", "2026-06-01T11:01:00.000Z");
  CHECK(edge.run(k_no_role).findings.empty());
  auto lapsed = edge.run(k_no_role, "2026-06-01T12:00:00.001Z");
  REQUIRE(lapsed.findings.size() == 1);
  // The lapsed claim's evidence time is its lease expiry.
  CHECK(lapsed.findings[0].evidence_times == std::vector<std::string>{"2026-06-01T12:00:00.000Z"});

  // An abandoned claim that is still marked active is as lapsed as it looks.
  fixture lapsed_claim;
  lapsed_claim.task(1, "doing");
  lapsed_claim.lapsed_claim(1, 1);
  lapsed_claim.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  CHECK(ids_of(lapsed_claim.run(k_no_role)) == std::vector<std::string>{"dispatch-no-role-action claim:1"});
}

TEST_CASE("dispatch-no-role-action is an event: bounded by the window and by the plan scope", "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done", 1);
  fx.task(2, "done", 2);
  fx.completed_claim(1, 1);
  fx.completed_claim(2, 2);
  fx.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  fx.preview(2, 2, "tok2", "2026-06-01T09:01:00.000Z");
  CHECK(ids_of(fx.run(k_no_role)) ==
        std::vector<std::string>{"dispatch-no-role-action claim:1", "dispatch-no-role-action claim:2"});
  CHECK(ids_of(fx.run(k_no_role, k_now, 2)) == std::vector<std::string>{"dispatch-no-role-action claim:2"});

  // The default window is seven days; a claim that ended before it is history.
  fixture old;
  old.task(1, "done");
  old.claim(1, 1, "completed", "2026-05-01T09:00:00.000Z", "2026-05-01T09:30:00.000Z", "2026-05-01T09:40:00.000Z",
            "2026-05-01T10:00:00.000Z");
  old.preview(1, 1, "tok1", "2026-05-01T09:01:00.000Z");
  CHECK(old.run(k_no_role).findings.empty());
  CHECK(old.run(k_no_role, k_now, std::nullopt, 60).findings.size() == 1);
}

TEST_CASE("dispatch-no-role-action keeps one fingerprint when a snapshot is recorded after the claim ended",
          "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done");
  fx.completed_claim(1, 1);
  fx.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  auto before = fx.run(k_no_role);
  REQUIRE(before.findings.size() == 1);

  fx.snapshot(1, 1, "2026-06-01T09:30:00.000Z");
  auto after = fx.run(k_no_role);
  REQUIRE(after.findings.size() == 1);
  CHECK(after.findings[0].evidence.size() == before.findings[0].evidence.size() + 1);
  CHECK(im::finding_fingerprint(before.findings[0]) == im::finding_fingerprint(after.findings[0]));
}

TEST_CASE("dispatch-no-role-action times its evidence from the claim, not from the evaluation instant",
          "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "done");
  fx.completed_claim(1, 1);
  fx.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  auto first = fx.run(k_no_role);
  auto later = fx.run(k_no_role, "2026-06-02T12:00:00.000Z");
  REQUIRE(first.findings.size() == 1);
  REQUIRE(later.findings.size() == 1);
  CHECK(first.findings[0].evidence_times == later.findings[0].evidence_times);
  CHECK(im::finding_digest(first.findings[0]) == im::finding_digest(later.findings[0]));
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
  CHECK(f.recovery.contains("planar-agent reconcile"));
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

TEST_CASE("action-unended is a state check: the window does not hide an old action, the plan scope does",
          "[engine][diagnose][actions]") {
  fixture fx;
  fx.task(1, "doing", 1);
  fx.task(2, "doing", 2);
  fx.claim(1, 1, "active", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z");
  fx.claim(2, 2, "active", "2026-04-01T09:00:00.000Z", "2026-04-01T09:30:00.000Z", "2026-04-01T09:40:00.000Z");
  fx.action(1, 1, "tool_call", 1, "2026-04-01T09:10:00.000Z");
  fx.action(2, 2, "tool_call", 2, "2026-04-01T09:10:00.000Z");
  CHECK(ids_of(fx.run({"action-unended"})) == std::vector<std::string>{"action-unended action:1", "action-unended action:2"});
  CHECK(ids_of(fx.run({"action-unended"}, k_now, 2)) == std::vector<std::string>{"action-unended action:2"});
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

TEST_CASE("a dispatch whose lapsed claim left an action open reports both checks", "[engine][diagnose][dispatch]") {
  fixture fx;
  fx.task(1, "doing");
  fx.lapsed_claim(1, 1);
  fx.preview(1, 1, "tok1", "2026-06-01T09:01:00.000Z");
  fx.action(1, 1, "tool_call", 1, "2026-06-01T09:10:00.000Z");
  CHECK(ids_of(fx.run({"dispatch-no-role-action", "action-unended"})) ==
        std::vector<std::string>{"dispatch-no-role-action claim:1", "action-unended action:1"});
}
