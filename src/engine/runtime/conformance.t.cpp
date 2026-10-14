// conformance.t.cpp: `planar.engine.runtime.conformance`, the read-only trail checker
// (plan 1142, task mo-conformance-checker; test spec 695, "the checker flags each trail
// violation").
//
// Every fixture is raw rows in a per-test migrated database: the checker reads what the verbs
// recorded, and a raw row is the only way to produce the trails the verbs refuse to write (a
// stale claim, a NULL-verdict review, a claim with no model). Each rule gets one fixture that
// breaks exactly it and asserts the violation list is that one entry.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.completion;
import planar.engine.runtime.conformance;

namespace {

namespace cf   = planar::engine::runtime::conformance;
namespace comp = planar::engine::runtime::completion;
namespace aa   = planar::engine::runtime::agentactivity;

constexpr std::string_view k_before  = "2025-12-01T00:00:00.000Z"; // before migration 42
constexpr std::string_view k_cutover = "2026-01-01T00:00:00.000Z"; // migration 42 applied
constexpr std::string_view k_after   = "2026-02-01T00:00:00.000Z"; // after migration 42
constexpr std::string_view k_later   = "2026-03-01T00:00:00.000Z";
constexpr std::string_view k_now     = "2026-06-01T00:00:00.000Z";
constexpr std::string_view k_future  = "2026-12-01T00:00:00.000Z";

struct scratch_dir {
  std::filesystem::path path;

  scratch_dir()
      : path(std::filesystem::temp_directory_path() / std::format("planar_conformance_test_{}_{}",
                                                                  std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                  reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path);
  }
  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

auto scalar_int(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// A database with plan 1 (the graded plan) and a second session for reviewers.
struct world {
  scratch_dir            dir;
  planar::db::connection conn = [this] {
    auto opened = planar::db::connection::open((dir.path / "planar.db").string());
    REQUIRE(opened.has_value());
    REQUIRE(planar::db::apply_all(*opened).has_value());
    return std::move(*opened);
  }();
  std::int64_t                coder_session    = 0;
  std::int64_t                reviewer_session = 0;
  int                         next_token       = 0;
  cf::policy_view             policy;
  std::vector<comp::gate_run> runs;

  world() {
    exec(conn, "insert into sessions (vendor) values ('test'), ('test')");
    coder_session    = 1;
    reviewer_session = 2;
    exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','p','anchor','active')");
    exec(conn, std::format("update schema_migrations set applied_at = '{}' where version = 42", k_cutover));
    policy.digest = "current-digest";
  }

  auto add_task(std::string_view status, std::string_view updated_at, std::int64_t plan_id = 1) -> std::int64_t {
    exec(conn, std::format("insert into tasks (scope_kind, plan_id, title, status, priority, updated_at) "
                           "values ('global', {}, 't', '{}', 100, '{}')",
                           plan_id, status, updated_at));
    return scalar_int(conn, "select max(id) from tasks");
  }

  struct claim_spec {
    std::string                status           = "completed";
    std::optional<std::string> role             = "coder";
    std::optional<std::string> model            = "m";
    std::optional<std::string> digest           = std::nullopt;
    std::string                claimed_at       = std::string{k_after};
    std::string                lease_expires_at = std::string{k_future};
    std::optional<std::string> repo_root        = "/work/repo";
    bool                       with_parent      = true;
  };

  auto text_or_null(const std::optional<std::string>& value) -> std::string {
    return value.has_value() ? std::format("'{}'", *value) : "null";
  }

  /// A claim on `task_id`; returns the claim row id.
  auto add_claim(std::int64_t task_id, const claim_spec& spec) -> std::int64_t {
    auto const token = std::format("{:032x}", ++next_token);
    exec(conn,
         std::format("insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, role, "
                     "model, policy_digest, claimed_at, lease_expires_at, repo_root, released_at) "
                     "values ('{}', {}, 'task', {}, '{}', 'test', {}, {}, {}, '{}', '{}', {}, {})",
                     token, coder_session, task_id, spec.status, text_or_null(spec.role), text_or_null(spec.model),
                     text_or_null(spec.digest), spec.claimed_at, spec.lease_expires_at, text_or_null(spec.repo_root),
                     spec.status == "active" ? "null" : "'2026-02-02T00:00:00.000Z'"));
    auto const id = scalar_int(conn, "select max(id) from agent_work_claims");
    if (spec.with_parent) {
      exec(conn, "insert into agent_actions (session_id, action_kind, vendor) values (1, 'orchestrator', 'test')");
      auto const parent = scalar_int(conn, "select max(id) from agent_actions");
      exec(conn, std::format("insert into agent_actions (session_id, claim_id, parent_action_id, action_kind, vendor) "
                             "values (1, {}, {}, 'coder', 'test')",
                             id, parent));
    } else {
      exec(conn,
           std::format("insert into agent_actions (session_id, claim_id, action_kind, vendor) values (1, {}, 'coder', 'test')",
                       id));
    }
    return id;
  }

  /// A review row about `claim_id`, written by `session`; `verdict` is `'approve'` or `null`.
  void add_review(std::int64_t claim_id, std::int64_t session, std::string_view verdict) {
    exec(conn, std::format("insert into agent_actions (session_id, claim_id, action_kind, vendor, verdict) "
                           "values ({}, {}, 'review', 'test', {})",
                           session, claim_id, verdict));
  }

  /// A done task with a completed claim, parent action and a model: nothing to flag.
  auto add_clean_task() -> std::int64_t {
    auto const task = add_task("done", k_after);
    add_claim(task, claim_spec{});
    return task;
  }

  auto lookup() -> comp::gate_run_lookup {
    return [this](planar::db::connection&, std::string_view) -> std::expected<std::vector<comp::gate_run>, aa::agent_error> {
      return runs;
    };
  }

  auto grade(std::optional<std::int64_t> floor = std::nullopt) -> cf::report {
    auto const resolver = [this](planar::db::connection&, std::int64_t) -> std::expected<cf::policy_view, std::string> {
      return policy;
    };
    auto result =
        cf::check(conn, cf::options{.plan_id = 1, .floor_claim_id = floor, .now = std::string{k_now}}, resolver, lookup());
    REQUIRE(result.has_value());
    return std::move(*result);
  }
};

/// The violation list as `rule:kind:id` strings, for one-line assertions.
auto summary(const cf::report& report) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& item : report.violations) {
    out.push_back(std::format("{}:{}:{}", cf::rule_number(item.broken), item.entity_kind, item.entity_id));
  }
  return out;
}

using strings = std::vector<std::string>;

} // namespace

TEST_CASE("a clean plan has no violations, no overrides and counts nothing before the floor", "[conformance][1142]") {
  world w;
  w.add_clean_task();
  w.add_clean_task();
  auto const report = w.grade();
  CHECK(report.conforms());
  CHECK(report.violations.empty());
  CHECK(report.overrides.empty());
  CHECK(report.policy_changed.empty());
  CHECK(report.claims_graded == 2);
  CHECK(report.done_tasks_graded == 2);
  CHECK(report.before.claims == 0);
  CHECK(report.before.done_tasks == 0);
  CHECK(report.plans == 1);
}

TEST_CASE("rule 1: a done task with no claim is exactly one violation", "[conformance][1142]") {
  world      w;
  auto const lonely = w.add_task("done", k_after);
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("1:task:{}", lonely)});
  CHECK(report.violations[0].reason.contains("no claim was ever taken"));
  CHECK(cf::rule_name(report.violations[0].broken) == "completed-claim");
}

TEST_CASE("rule 1: a done task whose only claim was released is a violation that names how the claim ended",
          "[conformance][1142]") {
  world      w;
  auto const task = w.add_task("done", k_after);
  w.add_claim(task, world::claim_spec{.status = "released"});
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("1:task:{}", task)});
  CHECK(report.violations[0].reason.contains("released"));
}

TEST_CASE("rule 1: an operator --force status change is an override, not a violation", "[conformance][1142]") {
  world      w;
  auto const forced = w.add_task("done", k_after);
  exec(w.conn, std::format("insert into audit_log (verb, entity_kind, entity_id, summary) "
                           "values ('status_change', 'task', {}, 'done (--force)')",
                           forced));
  // A plain `done` row is not an override: it is the audit row any `task done` writes.
  auto const plain = w.add_task("done", k_after);
  exec(w.conn, std::format("insert into audit_log (verb, entity_kind, entity_id, summary) "
                           "values ('status_change', 'task', {}, 'done')",
                           plain));
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("1:task:{}", plain)});
  REQUIRE(report.overrides.size() == 1);
  CHECK(report.overrides[0].task_id == forced);
  CHECK(report.overrides[0].reason.contains("--force"));
  CHECK_FALSE(report.conforms());
}

TEST_CASE("rule 2: a stale claim is exactly one violation", "[conformance][1142]") {
  world      w;
  auto const task   = w.add_task("doing", k_after);
  auto const stale  = w.add_claim(task, world::claim_spec{.status = "stale"});
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("2:claim:{}", stale)});
  CHECK(report.violations[0].claim_token.has_value());
  CHECK(report.violations[0].claimed_entity == std::format("task:{}", task));
}

TEST_CASE("rule 2: an active claim past its lease is a violation and a live one is work in progress", "[conformance][1142]") {
  world      w;
  auto const lapsed =
      w.add_claim(w.add_task("doing", k_after), world::claim_spec{.status = "active", .lease_expires_at = std::string{k_later}});
  w.add_claim(w.add_task("doing", k_after), world::claim_spec{.status = "active", .lease_expires_at = std::string{k_future}});
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("2:claim:{}", lapsed)});
  CHECK(report.violations[0].reason.contains("past its lease"));
}

TEST_CASE("rule 2: released and aborted claims ended in a terminal verb", "[conformance][1142]") {
  world w;
  w.add_claim(w.add_task("todo", k_after), world::claim_spec{.status = "released"});
  w.add_claim(w.add_task("todo", k_after), world::claim_spec{.status = "aborted"});
  CHECK(w.grade().conforms());
}

TEST_CASE("rule 3: a coder claim with no model under require_model is exactly one violation", "[conformance][1142]") {
  world w;
  w.policy.require_model = true;
  w.add_clean_task();
  auto const task     = w.add_task("done", k_after);
  auto const nameless = w.add_claim(task, world::claim_spec{.model = std::nullopt});
  auto const report   = w.grade();
  CHECK(summary(report) == strings{std::format("3:claim:{}", nameless)});

  // The same claim is fine when the policy does not ask for a model, and an empty model is no model.
  w.policy.require_model = false;
  CHECK(w.grade().conforms());
  w.policy.require_model = true;
  exec(w.conn, std::format("update agent_work_claims set model = '' where id = {}", nameless));
  CHECK(summary(w.grade()) == strings{std::format("3:claim:{}", nameless)});
}

TEST_CASE("rule 3: a role other than coder needs no model", "[conformance][1142]") {
  world w;
  w.policy.require_model = true;
  w.add_claim(w.add_task("done", k_after), world::claim_spec{.role = "reviewer", .model = std::nullopt});
  CHECK(w.grade().conforms());
}

TEST_CASE("rule 4: a worker claim with no parent action is exactly one violation", "[conformance][1142]") {
  world w;
  w.add_clean_task();
  auto const orphan = w.add_claim(w.add_task("done", k_after), world::claim_spec{.with_parent = false});
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("4:claim:{}", orphan)});
}

TEST_CASE("rule 4: a review row's parent does not stand in for the claim's own, and orchestrator and roleless claims are exempt",
          "[conformance][1142]") {
  world      w;
  auto const orphan = w.add_claim(w.add_task("done", k_after), world::claim_spec{.with_parent = false});
  // A review is an action ABOUT the claim; its parent is the reviewed claim's action, not a dispatcher.
  exec(w.conn, "insert into agent_actions (session_id, action_kind, vendor) values (1, 'orchestrator', 'test')");
  exec(w.conn, std::format("insert into agent_actions (session_id, claim_id, parent_action_id, action_kind, vendor, verdict) "
                           "values (2, {}, (select max(id) from agent_actions), 'review', 'test', 'approve')",
                           orphan));
  w.add_claim(w.add_task("todo", k_after), world::claim_spec{.status = "released", .role = "orchestrator", .with_parent = false});
  w.add_claim(w.add_task("todo", k_after), world::claim_spec{.status = "released", .role = std::nullopt, .with_parent = false});
  CHECK(summary(w.grade()) == strings{std::format("4:claim:{}", orphan)});
}

TEST_CASE("rule 5: a completed claim with no queued gate run is exactly one violation, and a matching run satisfies it",
          "[conformance][1142]") {
  world w;
  w.policy.use_queue = true;
  w.policy.task_gate = std::vector<std::string>{"make", "test"};
  auto const task    = w.add_task("done", k_after);
  auto const claim   = w.add_claim(task, world::claim_spec{});
  auto const report  = w.grade();
  CHECK(summary(report) == strings{std::format("5:claim:{}", claim)});
  CHECK(report.violations[0].reason.contains("task_gate"));

  // A run of another argv, or one that failed, is not the gate.
  w.runs = {comp::gate_run{.argv = {"make", "test-all"}, .cwd = "/work/repo", .passed = true},
            comp::gate_run{.argv = {"make", "test"}, .cwd = "/work/repo", .passed = false}};
  CHECK(summary(w.grade()) == strings{std::format("5:claim:{}", claim)});

  w.runs = {comp::gate_run{.argv = {"make", "test"}, .cwd = "/work/repo", .passed = true}};
  CHECK(w.grade().conforms());
}

TEST_CASE("rule 5: a gate with no use_queue is not graded", "[conformance][1142]") {
  world w;
  w.policy.task_gate = std::vector<std::string>{"make", "test"};
  w.add_clean_task();
  CHECK(w.grade().conforms());
}

TEST_CASE("rule 6: a completed claim whose review came from its own session is exactly one violation", "[conformance][1142]") {
  world w;
  w.policy.review_required = true;
  auto const task          = w.add_task("done", k_after);
  auto const claim         = w.add_claim(task, world::claim_spec{});
  w.add_review(claim, w.coder_session, "'approve'");
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("6:claim:{}", claim)});
  CHECK(report.violations[0].reason.contains("own session"));
}

TEST_CASE("rule 6: a review from another session with a verdict satisfies the rule", "[conformance][1142]") {
  world w;
  w.policy.review_required = true;
  auto const claim         = w.add_claim(w.add_task("done", k_after), world::claim_spec{});
  w.add_review(claim, w.reviewer_session, "'approve'");
  CHECK(w.grade().conforms());
}

TEST_CASE("rule 6: a NULL-verdict review from another session never satisfies the rule", "[conformance][1142]") {
  world w;
  w.policy.review_required = true;
  auto const claim         = w.add_claim(w.add_task("done", k_after), world::claim_spec{});
  w.add_review(claim, w.reviewer_session, "null");
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("6:claim:{}", claim)});
  CHECK(report.violations[0].reason.contains("no review"));
}

TEST_CASE("rule 6: a claim that did not complete needs no review", "[conformance][1142]") {
  world w;
  w.policy.review_required = true;
  w.add_claim(w.add_task("todo", k_after), world::claim_spec{.status = "released"});
  CHECK(w.grade().conforms());
}

TEST_CASE("claims and done tasks before the floor never fail and are counted", "[conformance][1142]") {
  world w;
  // Before migration 42: a stale claim, a coder with no model, no parent action, and an unclaimed done task.
  w.policy.require_model = true;
  w.add_claim(
      w.add_task("doing", k_before),
      world::claim_spec{.status = "stale", .model = std::nullopt, .claimed_at = std::string{k_before}, .with_parent = false});
  w.add_task("done", k_before);
  w.add_task("done", k_before);
  // After it: one clean task.
  auto const kept   = w.add_clean_task();
  auto const report = w.grade();
  CHECK(report.conforms());
  CHECK(report.before.claims == 1);
  CHECK(report.before.done_tasks == 2);
  CHECK(report.claims_graded == 1);
  CHECK(report.done_tasks_graded == 1);
  CHECK(report.floor.origin == cf::floor_origin::migration);
  CHECK(report.floor.cut_at == std::string{k_cutover});
  CHECK(report.floor.claim_id ==
        scalar_int(w.conn,
                   std::format("select min(id) from agent_work_claims where status = 'completed' and entity_id = {}", kept)));

  const auto text = cf::render_text(report);
  CHECK(text.contains("before the floor: 1 claims, 2 done tasks"));
}

TEST_CASE("the default floor is one past the highest claim when nothing was claimed after migration 42", "[conformance][1142]") {
  world w;
  w.add_claim(w.add_task("doing", k_before), world::claim_spec{.status = "stale", .claimed_at = std::string{k_before}});
  auto const floor = cf::resolve_floor(w.conn, std::nullopt);
  REQUIRE(floor.has_value());
  CHECK(floor->claim_id == 2);
  CHECK(floor->origin == cf::floor_origin::migration);
  CHECK(w.grade().conforms());
}

TEST_CASE("without a record of migration 42 the floor is the first claim that recorded a policy digest", "[conformance][1142]") {
  world w;
  exec(w.conn, "update schema_migrations set applied_at = applied_at where version = 42");
  exec(w.conn, "delete from schema_migrations where version = 42");
  w.add_claim(w.add_task("doing", k_before), world::claim_spec{.status = "stale", .claimed_at = std::string{k_before}});
  auto const digested =
      w.add_claim(w.add_task("doing", k_after), world::claim_spec{.status = "stale", .digest = "current-digest"});
  auto const report = w.grade();
  CHECK(report.floor.origin == cf::floor_origin::digest);
  CHECK(report.floor.claim_id == digested);
  CHECK(summary(report) == strings{std::format("2:claim:{}", digested)});
  CHECK(report.before.claims == 1);
}

TEST_CASE("an explicit floor replaces the data-derived one", "[conformance][1142]") {
  world      w;
  auto const early = w.add_claim(w.add_task("doing", k_after), world::claim_spec{.status = "stale"});
  auto const late =
      w.add_claim(w.add_task("doing", k_later), world::claim_spec{.status = "stale", .claimed_at = std::string{k_later}});
  CHECK(summary(w.grade()) == strings{std::format("2:claim:{}", early), std::format("2:claim:{}", late)});

  auto const report = w.grade(late);
  CHECK(report.floor.origin == cf::floor_origin::explicit_);
  CHECK(report.floor.claim_id == late);
  CHECK(report.floor.cut_at == std::string{k_later});
  CHECK(summary(report) == strings{std::format("2:claim:{}", late)});
  CHECK(report.before.claims == 1);

  // A floor past every claim grades no claim and no task.
  w.add_task("done", k_later);
  auto const beyond = w.grade(late + 10);
  CHECK(beyond.conforms());
  CHECK(beyond.claims_graded == 0);
  CHECK_FALSE(beyond.floor.cut_at.has_value());
  CHECK(beyond.before.done_tasks == 1);

  // A floor at or below the first claim grades everything, tasks that predate every claim included.
  w.add_task("done", k_before);
  auto const all = w.grade(1);
  CHECK(all.floor.cut_at == std::string{});
  CHECK(all.done_tasks_graded == 2);
}

TEST_CASE("descendant plans are graded and unrelated plans are not", "[conformance][1142]") {
  world w;
  exec(w.conn,
       "insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global','child','child','active', 1)");
  exec(w.conn, "insert into plans (scope_kind, title, slug, status) values ('global','other','other','active')");
  auto const child_task = w.add_task("done", k_after, 2);
  w.add_task("done", k_after, 3);
  auto const report = w.grade();
  CHECK(report.plans == 2);
  CHECK(summary(report) == strings{std::format("1:task:{}", child_task)});
}

TEST_CASE("a claim whose policy digest differs is graded on the policy-independent rules only", "[conformance][1142]") {
  world w;
  w.policy.require_model = true;
  // Ran under another policy: its missing model is not held against the current one, its missing parent is.
  auto const older = w.add_claim(w.add_task("done", k_after),
                                 world::claim_spec{.model = std::nullopt, .digest = "older-digest", .with_parent = false});
  // Ran under the current policy: the missing model counts.
  auto const current =
      w.add_claim(w.add_task("done", k_after), world::claim_spec{.model = std::nullopt, .digest = "current-digest"});
  auto const report = w.grade();
  CHECK(summary(report) == strings{std::format("3:claim:{}", current), std::format("4:claim:{}", older)});
  REQUIRE(report.policy_changed.size() == 1);
  CHECK(report.policy_changed[0].claim_id == older);
}

TEST_CASE("a policy that cannot be resolved refuses the check and names why", "[conformance][1142]") {
  world w;
  w.add_clean_task();
  auto const refuse = [](planar::db::connection&, std::int64_t) -> std::expected<cf::policy_view, std::string> {
    return std::unexpected(std::string{"config.toml is not valid"});
  };
  auto const result = cf::check(w.conn, cf::options{.plan_id = 1}, refuse, w.lookup());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind == cf::error_kind::policy_unavailable);
  CHECK(result.error().detail == "config.toml is not valid");
}

TEST_CASE("an unknown plan is refused", "[conformance][1142]") {
  world      w;
  auto const resolver = [&](planar::db::connection&, std::int64_t) -> std::expected<cf::policy_view, std::string> {
    return w.policy;
  };
  auto const result = cf::check(w.conn, cf::options{.plan_id = 99}, resolver, w.lookup());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind == cf::error_kind::plan_not_found);
}

TEST_CASE("the text and JSON renderings carry rule, entity ids and the override and floor sections", "[conformance][1142]") {
  world      w;
  auto const lonely = w.add_task("done", k_after);
  auto const forced = w.add_task("done", k_after);
  exec(w.conn, std::format("insert into audit_log (verb, entity_kind, entity_id, summary) "
                           "values ('status_change', 'task', {}, 'done (--force)')",
                           forced));
  auto const report = w.grade();

  auto const text = cf::render_text(report);
  CHECK(text.contains("violations: 1"));
  CHECK(text.contains(std::format("rule 1 completed-claim: task {}", lonely)));
  CHECK(text.contains("overrides: 1"));
  CHECK(text.contains(std::format("task {}: operator moved the task to done with --force", forced)));
  CHECK(text.contains("before the floor: 0 claims, 0 done tasks"));

  auto const json = cf::render_json(report);
  CHECK(json.contains("\"conforms\":false"));
  CHECK(json.contains(std::format("\"entity_kind\":\"task\",\"entity_id\":{}", lonely)));
  CHECK(json.contains("\"name\":\"completed-claim\""));
  CHECK(json.contains(std::format("\"overrides\":[{{\"task_id\":{}", forced)));
  CHECK(json.contains("\"before_floor\":{\"claims\":0,\"done_tasks\":0}"));
  CHECK(json.ends_with("}\n"));
}
