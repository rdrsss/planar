// @file diagnose_checks.t.cpp
// @brief Claim-liveness diagnose checks through the real binaries (plan 1132, tasks 7374 and 7375).
//
// Every claim here is written by `planar-agent` itself: `claim`, `heartbeat`, `complete`,
// `abort` and `reconcile` run against a scratch database, and `planar task update` makes the
// states an operator can reach. The one thing the verbs cannot do is wait: a lease is minutes
// long. Where a case needs a lapsed lease or a gap between heartbeats, it moves a timestamp
// column afterwards with a plain UPDATE (`move_time`), and says so at the call. No row is
// inserted by hand.
//
// What is pinned:
//
//   * A claim that never heartbeated and lapsed is `claim-process-died`; after `reconcile` marks
//     it stale while its task stays `doing` (a `--no-transition` claim has no action row to prove
//     it flipped the task), the findings become `task-doing-unclaimed` plus the info event
//     `claim-closed-by-reconcile`.
//   * A claim that heartbeated and then lapsed is `claim-lease-lapsed`; `reconcile` then returns
//     the task to `todo` and leaves only the info event.
//   * A claim stranded on a task the operator forced to `done` is `claim-superseded-active`,
//     and `abort` clears it.
//   * A healthy claim, before and after `complete`, gives no claim finding.
//   * `heartbeat-gap` reads the heartbeat action rows the verb writes.
//   * `handoff-stale` agrees with the stale-handoff count `planar report` prints.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.json_dom;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;

struct world {
  parity::arena         arena = parity::make_arena("watch_diagnose_checks");
  std::filesystem::path root  = arena.cpp_root;
  std::filesystem::path op    = std::filesystem::path{PLANAR_OPERATOR_CPP_BIN};
  std::filesystem::path agent = std::filesystem::path{PLANAR_AGENT_CPP_BIN};
  std::filesystem::path watch = std::filesystem::path{PLANAR_CPP_BIN};
  int                   seq   = 0;

  auto tag() -> std::string {
    return std::format("c{}", ++seq);
  }

  auto planar(std::vector<std::string> args) -> parity::capture {
    auto got = parity::run_pinned(op, args, root, tag());
    INFO("planar " << args.front() << ": " << got.err);
    REQUIRE(got.code == 0);
    return got;
  }

  auto planar_agent(std::vector<std::string> args) -> parity::capture {
    auto got = parity::run_pinned(agent, args, root, tag());
    INFO("planar-agent " << args.front() << ": " << got.err << got.out);
    REQUIRE(got.code == 0);
    return got;
  }

  world() {
    planar({"init"});
    planar({"assoc", "create", "project:proj", "--kind", "project"});
    planar({"assoc", "add", "project:proj", (root / "proj").string()});
    planar({"plan", "create", "Demo plan"});
    planar({"task", "add", "First task", "--plan", "1"});
    planar({"task", "add", "Second task", "--plan", "1"});
    planar({"plan", "update", "1", "--status", "active"});
  }

  /// Claims a task through the real verb and returns the claim token.
  auto claim(int task_id, bool no_transition = false) -> std::string {
    std::vector<std::string> args{
        "claim", "--entity",           std::format("task:{}", task_id), "--vendor", "claude", "--role", "coder", "--ttl",
        "30m",   "--no-locality-probe"};
    if (no_transition) {
      args.push_back("--no-transition");
    }
    auto        got  = planar_agent(args);
    std::string text = got.out;
    auto        at   = text.find("claim:");
    REQUIRE(at != std::string::npos);
    auto start = at + 6;
    return text.substr(start, text.find(' ', start) - start);
  }

  /// TIME TRAVEL: the verbs cannot wait out a lease, so a timestamp column is moved afterwards.
  /// This is the only write the test makes outside a verb.
  auto move_time(std::string_view sql) -> void {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto ok = conn->execute(sql);
    INFO(sql);
    REQUIRE(ok.has_value());
  }

  /// Findings of `planar-watch diagnose --plan 1 --json` as `check-id entity severity` strings.
  auto findings(std::vector<std::string> checks, std::string_view days = {}) -> std::vector<std::string> {
    std::vector<std::string> args{"diagnose", "--plan", "1", "--json"};
    if (!days.empty()) {
      args.push_back("--days");
      args.emplace_back(days);
    }
    for (auto& c : checks) {
      args.push_back("--check");
      args.push_back(std::move(c));
    }
    auto got = parity::run_pinned(watch, args, root, tag());
    INFO("diagnose stderr: " << got.err);
    REQUIRE(got.code == 0);
    auto parsed = planar::json_dom::parse_json(got.out);
    REQUIRE(parsed.has_value());
    CHECK(parsed->find("outcome")->string == "ok");
    std::vector<std::string> out;
    for (const auto& f : parsed->find("findings")->array) {
      out.push_back(std::format("{} {} {}", f.find("check")->string, f.find("entity")->string, f.find("severity")->string));
    }
    return out;
  }
};

const std::vector<std::string> k_claim_checks{"claim-lease-lapsed", "claim-process-died", "claim-superseded-active",
                                              "task-doing-unclaimed", "claim-closed-by-reconcile"};

} // namespace

TEST_CASE("a claim reconciled stale while its task stays doing is seen from process death to unclaimed task",
          "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  // The operator moves the task to doing; the claim is taken with --no-transition and never
  // heartbeats, so it has no action row and reconcile cannot prove it flipped the task.
  w.planar({"task", "update", "1", "--status", "doing"});
  w.claim(1, true);
  CHECK(w.findings(k_claim_checks).empty());

  // TIME TRAVEL: the lease lapsed long ago.
  w.move_time("update agent_work_claims set lease_expires_at = '2020-01-01T00:10:00.000Z'");
  // The lapsed lease also leaves the doing task without an unexpired claim.
  auto dead = w.findings(k_claim_checks);
  CHECK(std::ranges::contains(dead, std::string{"claim-process-died claim:1 warning"}));
  CHECK(std::ranges::contains(dead, std::string{"task-doing-unclaimed task:1 warning"}));
  CHECK(dead.size() == 2);

  w.planar_agent({"reconcile"});
  auto after = w.findings(k_claim_checks);
  CHECK(std::ranges::contains(after, std::string{"task-doing-unclaimed task:1 warning"}));
  CHECK(std::ranges::contains(after, std::string{"claim-closed-by-reconcile claim:1 info"}));
  CHECK(after.size() == 2);
}

TEST_CASE("a claim that heartbeated and lapsed is a lapsed lease until reconcile returns the task",
          "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  auto  token = w.claim(1);
  w.planar_agent({"heartbeat", "--claim", token, "--status", "editing"});
  CHECK(w.findings(k_claim_checks).empty());

  // TIME TRAVEL: the lease lapsed long ago.
  w.move_time("update agent_work_claims set lease_expires_at = '2020-01-01T00:10:00.000Z'");
  auto lapsed = w.findings(k_claim_checks);
  CHECK(std::ranges::contains(lapsed, std::string{"claim-lease-lapsed claim:1 warning"}));
  CHECK(std::ranges::contains(lapsed, std::string{"task-doing-unclaimed task:1 warning"}));
  CHECK(lapsed.size() == 2);
  CHECK(w.findings({"claim-process-died"}).empty());

  // Reconcile marks the claim stale and, with an action row on it, returns the task to todo.
  w.planar_agent({"reconcile"});
  CHECK(w.findings(k_claim_checks) == std::vector<std::string>{"claim-closed-by-reconcile claim:1 info"});
}

TEST_CASE("a claim stranded on a task forced to done is superseded until aborted", "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  auto  token = w.claim(1);
  w.planar({"task", "update", "1", "--status", "done", "--force"});
  CHECK(w.findings(k_claim_checks) == std::vector<std::string>{"claim-superseded-active claim:1 error"});

  w.planar_agent({"abort", "--claim", token});
  CHECK(w.findings(k_claim_checks).empty());
}

TEST_CASE("a healthy claim gives no claim finding before or after complete", "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  auto  token = w.claim(1);
  w.planar_agent({"heartbeat", "--claim", token});
  CHECK(w.findings(k_claim_checks).empty());
  w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});
  CHECK(w.findings(k_claim_checks).empty());
}

TEST_CASE("heartbeat gaps are read from the heartbeat rows the verb writes", "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  auto  token = w.claim(1);
  w.planar_agent({"heartbeat", "--claim", token, "--status", "one"});
  w.planar_agent({"heartbeat", "--claim", token, "--status", "two"});
  // Back to back, the real heartbeats leave no gap.
  CHECK(w.findings({"heartbeat-gap"}).empty());

  // TIME TRAVEL: the verbs cannot wait out a lease. The claim was taken with a 30 minute lease; the
  // heartbeats are moved to 20 and then 60 minutes after claiming, so the first gap (20 minutes)
  // is beyond half the lease (info) and the second (40 minutes) is beyond the whole lease (warning).
  w.move_time("update agent_work_claims set claimed_at = '2020-01-01T00:00:00.000Z'");
  w.move_time("update agent_actions set started_at = '2020-01-01T00:20:00.000Z', ended_at = '2020-01-01T00:20:00.000Z' "
              "where action_kind = 'heartbeat' and summary = 'one'");
  w.move_time("update agent_actions set started_at = '2020-01-01T01:00:00.000Z', ended_at = '2020-01-01T01:00:00.000Z' "
              "where action_kind = 'heartbeat' and summary = 'two'");
  CHECK(w.findings({"heartbeat-gap"}, "36500") ==
        std::vector<std::string>{"heartbeat-gap claim:1 warning", "heartbeat-gap claim:1 info"});
}

TEST_CASE("handoff-stale agrees with the stale-handoff count planar report prints", "[cmd][watch][diagnose][workflow][handoff]") {
  world w;
  // Handoffs need an open session; the pinned environment is extended with its identity.
  auto env = parity::pinned_env(w.root);
  env.push_back(parity::pinned_var{.name = "PLANAR_VENDOR", .value = "claude"});
  env.push_back(parity::pinned_var{.name = "PLANAR_VENDOR_SESSION_ID", .value = "s1"});
  auto op = [&](std::vector<std::string> args) {
    auto got = parity::run_pinned(w.op, args, w.root, w.tag(), env);
    INFO("planar " << args.front() << ": " << got.err);
    REQUIRE(got.code == 0);
    return got;
  };
  op({"capture", "session", "--task", "1"});
  op({"handoff", "1"});
  op({"handoff", "2"});
  op({"handoff", "1"});
  op({"handoff", "consume", "3"});
  CHECK(w.findings({"handoff-stale"}).empty());

  // TIME TRAVEL: age the handoffs, since a stale handoff is a day old. 1 is 25 h old and pending
  // review, 2 is 23 h old, 3 is 25 h old but consumed.
  w.move_time("update handoffs set created_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-25 hours') where id in (1, 3)");
  w.move_time("update handoffs set created_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-23 hours') where id = 2");
  CHECK(w.findings({"handoff-stale"}) == std::vector<std::string>{"handoff-stale handoff:1 warning"});

  auto report = op({"report", "--json"});
  auto parsed = planar::json_dom::parse_json(report.out);
  REQUIRE(parsed.has_value());
  CHECK(parsed->find("handoffs")->find("stale_handoffs")->integer == 1);
}
