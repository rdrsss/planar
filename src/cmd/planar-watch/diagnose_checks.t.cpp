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
//   * Dispatch checks (plan 1132, tasks 7376 and 7377): the dispatch records come from
//     `planar-agent dispatch preview` and `confirm`, which take caller-supplied digests (GitHub
//     issue #247), so a test passes consistent synthetic ones. The project and routing candidate a
//     preview names are the only rows seeded by hand; no verb creates them.
//   * `dispatch-no-role-action`: the 7335 shape (a preview bound to a claim, a confirm, a complete,
//     no coder action) is one finding and nothing else; a direct claim doing its own work is none.
//   * `action-unended`: an action left open under a claim that lapsed.
//   * `apply-without-preview` (task 7379): the capture log is turned on through `[introspection]
//     cli_log` in the pinned config file, and `planar spec ingest` runs with and without `--apply`.
//     There is no workbench spec, so the ingest exits 2, which the log records all the same. With
//     the log off the check's input is `disabled`, the check does not run and the outcome stays `ok`;
//     a config file that cannot be parsed reads `unavailable` and the outcome `partial`.
//   * Failure clusters (task 7380): three `planar task add` calls with no title fail with a usage
//     error, the capture log records them, and `cli-failure-cluster` names `task add` and `usage`
//     with its three invocations; two failures, or three split across verbs, make none. Claims ended
//     with `planar-agent fail --category tool_failure` form one `claim-failure-cluster`; the default
//     category (`unknown`) forms none.
//   * `sync-conflict-unresolved` (task 7381): a local fake Jira (the in-process HTTP fixture server)
//     answers `planar-ext sync pull`; the remote and the local task title are both changed, so the pull
//     records a `conflict` event and exits 3, and the diagnosis names the event and its link. After
//     `planar-ext sync resolve` (the evidence token and the task's `updated_at` are read back from the
//     database, as an operator would from the evidence) the diagnosis reports nothing. A `sync pull`
//     that conflicts again after a resolution is reported again.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.json_dom;

#include "../lib/http/fixture_server.hpp"
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

  /// Turns the capture log on in the pinned config file every binary here reads (`PLANAR_CONFIG_PATH`).
  auto enable_cli_log() -> void {
    std::ofstream config{root / "config.toml", std::ios::binary};
    config << "[introspection]\ncli_log = true\n";
    REQUIRE(config.good());
  }

  /// Runs `planar` and returns its exit code without requiring success: an ingest with no spec fails, and is logged.
  auto planar_status(std::vector<std::string> args) -> int {
    return parity::run_pinned(op, args, root, tag()).code;
  }

  /// One integer from a read-only query over the scratch database.
  auto scalar(std::string_view sql) -> std::int64_t {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(sql);
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    return stmt->column_int64(0);
  }

  /// `planar-watch diagnose --plan 1 --json`, parsed, without requiring the outcome to be `ok`.
  auto diagnose_any(std::vector<std::string> checks) -> planar::json_dom::json_value {
    std::vector<std::string> args{"diagnose", "--plan", "1", "--json"};
    for (auto& c : checks) {
      args.push_back("--check");
      args.push_back(std::move(c));
    }
    auto got = parity::run_pinned(watch, args, root, tag());
    INFO("diagnose stderr: " << got.err);
    REQUIRE(got.code == 0);
    auto parsed = planar::json_dom::parse_json(got.out);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
  }

  /// `planar-ext` under the pinned environment plus the fake Jira's token; returns the capture whatever the exit code.
  auto ext(std::vector<std::string> args) -> parity::capture {
    auto env = parity::pinned_env(root);
    env.push_back(parity::pinned_var{.name = "DEMO_TOKEN", .value = "tok-abc"});
    auto got = parity::run_pinned(std::filesystem::path{PLANAR_EXT_CPP_BIN}, args, root, tag(), env);
    INFO("planar-ext " << args.front() << " " << args[1] << ": " << got.err << got.out);
    return got;
  }

  /// One text value from a read-only query over the scratch database.
  auto text(std::string_view sql) -> std::string {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(sql);
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    return stmt->column_text(0);
  }

  /// Rows no verb creates, written with a plain INSERT: the project and routing candidate a dispatch preview names.
  auto seed_routing() -> void {
    move_time("insert into projects (slug, name) values ('routing-project', 'Routing Project')");
    move_time("insert into routing_candidates (vendor, candidate_id, fallback_order) values ('claude', 'candidate', 0)");
  }

  /// The current instant as the whole-second RFC 3339 text a caller passes to `dispatch confirm --now`.
  static auto now_text(int offset_seconds = 0) -> std::string {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() +
                                                                                         std::chrono::seconds{offset_seconds}));
  }

  /// `planar-agent dispatch preview` for a task, bound to `claim_token` when given, with consistent synthetic digests.
  /// Returns the preview token.
  auto dispatch_preview(int task_id, std::string_view claim_token) -> std::string {
    std::vector<std::string> args{"dispatch",
                                  "preview",
                                  "--task",
                                  std::to_string(task_id),
                                  "--work-item",
                                  std::format("w{}", task_id),
                                  "--project",
                                  "1",
                                  "--validation-policy",
                                  "v1",
                                  "--routing-policy",
                                  "r1",
                                  "--profile-rule",
                                  "p1",
                                  "--vendor",
                                  "claude",
                                  "--role",
                                  "coder",
                                  "--tier",
                                  "medium",
                                  "--work-type",
                                  "feature",
                                  "--complexity",
                                  "standard",
                                  "--packet-digest",
                                  "pk",
                                  "--profile-digest",
                                  "pf",
                                  "--policy-digest",
                                  "po",
                                  "--capability-digest",
                                  "cp",
                                  "--candidate",
                                  "1",
                                  "--host",
                                  "host",
                                  "--class",
                                  "default",
                                  "--evidence-state",
                                  "evidential",
                                  "--expires-at",
                                  "2099-01-01T00:00:00Z"};
    if (!claim_token.empty()) {
      args.push_back("--claim");
      args.emplace_back(claim_token);
      args.push_back("--claim-status");
      args.push_back("active");
    }
    auto        got  = planar_agent(args);
    std::string text = got.out;
    auto        at   = text.find("preview:");
    REQUIRE(at != std::string::npos);
    auto start = text.find_first_not_of(' ', at + 8);
    return text.substr(start, text.find_first_of(" \n", start) - start);
  }

  /// `planar-agent dispatch confirm` of a preview, at `now` (RFC 3339), against the values the preview froze.
  auto dispatch_confirm(std::string_view preview_token, std::string_view now, std::string_view claim_token = {}) -> void {
    std::vector<std::string> args{"dispatch",
                                  "confirm",
                                  "--token",
                                  std::string{preview_token},
                                  "--dispatch-key",
                                  std::format("key-{}", preview_token),
                                  "--now",
                                  std::string{now},
                                  "--packet-digest",
                                  "pk",
                                  "--profile-digest",
                                  "pf",
                                  "--policy-digest",
                                  "po",
                                  "--capability-digest",
                                  "cp",
                                  "--candidate",
                                  "1",
                                  "--vendor",
                                  "claude",
                                  "--role",
                                  "coder",
                                  "--tier",
                                  "medium",
                                  "--work-type",
                                  "feature",
                                  "--complexity",
                                  "standard",
                                  "--validation-policy",
                                  "v1",
                                  "--routing-policy",
                                  "r1"};
    if (!claim_token.empty()) {
      args.push_back("--claim");
      args.emplace_back(claim_token);
      args.push_back("--claim-status");
      args.push_back("active");
    }
    planar_agent(args);
  }

  /// `planar-watch diagnose --plan 1 --json`, parsed.
  auto diagnose_json(std::vector<std::string> checks, std::string_view days = {}) -> planar::json_dom::json_value {
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
    return std::move(*parsed);
  }

  /// Findings of `planar-watch diagnose --plan 1 --json` as `check-id entity severity` strings.
  auto findings(std::vector<std::string> checks, std::string_view days = {}) -> std::vector<std::string> {
    auto                     parsed = diagnose_json(std::move(checks), days);
    std::vector<std::string> out;
    for (const auto& f : parsed.find("findings")->array) {
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
  // heartbeats, so it has no action row and reconcile cannot prove it flipped the task. (Reconcile
  // returns a task to todo only when the swept claim has an action row AND no other active,
  // unexpired claim holds the task; either condition failing leaves the task doing.)
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

  // Reconcile marks the claim stale and, with an action row on it and no other live claim on the
  // task, returns the task to todo.
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

TEST_CASE("a lapsed claim left active behind a recovery claim is superseded and names the later claim",
          "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  // A is taken and its lease lapses (TIME TRAVEL below); recovery takes B with --no-transition
  // (--force would mark A stale) and B finishes the task, leaving A active.
  auto first = w.claim(1);
  w.move_time("update agent_work_claims set lease_expires_at = '2020-01-01T00:10:00.000Z'");
  auto second = w.claim(1, true);
  w.planar_agent({"complete", "--claim", second, "--no-locality-probe"});
  (void)first;

  auto        parsed = w.diagnose_json({"claim-superseded-active"});
  const auto& found  = parsed.find("findings")->array;
  REQUIRE(found.size() == 1);
  CHECK(found[0].find("entity")->string == "claim:1");
  std::vector<std::string> evidence;
  for (const auto& e : found[0].find("evidence")->array) {
    evidence.push_back(e.string);
  }
  CHECK(std::ranges::is_permutation(evidence, std::vector<std::string>{"claim:1", "claim:2", "task:1"}));
  // A's lease expiry, then B's release time (a row timestamp, so it is later than 2020).
  REQUIRE(found[0].find("evidence_times")->array.size() == 2);
  CHECK(found[0].find("evidence_times")->array[0].string == "2020-01-01T00:10:00.000Z");
  CHECK(found[0].find("fingerprint")->string == "claim-superseded-active|claim:1|task:1|global");
}

TEST_CASE("a healthy claim gives no claim finding before or after complete", "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  auto  token = w.claim(1);
  w.planar_agent({"heartbeat", "--claim", token});
  CHECK(w.findings(k_claim_checks).empty());
  w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});
  CHECK(w.findings(k_claim_checks).empty());
}

TEST_CASE("heartbeat gaps: info between recorded heartbeats, a warning for the trailing stretch",
          "[cmd][watch][diagnose][workflow][claims]") {
  world w;
  auto  token = w.claim(1);
  w.planar_agent({"heartbeat", "--claim", token, "--status", "one"});
  w.planar_agent({"heartbeat", "--claim", token, "--status", "two"});
  // Back to back, the real heartbeats leave no gap and the lease is live.
  CHECK(w.findings({"heartbeat-gap"}).empty());

  // TIME TRAVEL: the verbs cannot wait out a lease. The claim has a 30 minute lease. It is moved
  // to a history a successful heartbeat sequence could have produced: claimed at 00:00, heartbeats
  // recorded at 00:20 and 00:45 (gaps of 20 and 25 minutes, each beyond half the lease but within
  // it), the last one renewing the lease to 01:15, and then silence.
  w.move_time("update agent_work_claims set claimed_at = '2020-01-01T00:00:00.000Z', "
              "last_heartbeat_at = '2020-01-01T00:45:00.000Z', lease_expires_at = '2020-01-01T01:15:00.000Z'");
  w.move_time("update agent_actions set started_at = '2020-01-01T00:20:00.000Z', ended_at = '2020-01-01T00:20:00.000Z' "
              "where action_kind = 'heartbeat' and summary = 'one'");
  w.move_time("update agent_actions set started_at = '2020-01-01T00:45:00.000Z', ended_at = '2020-01-01T00:45:00.000Z' "
              "where action_kind = 'heartbeat' and summary = 'two'");
  // The two gaps between recorded points are info; the silence after the last heartbeat, past the
  // lease, is the one warning.
  CHECK(w.findings({"heartbeat-gap"}, "36500") ==
        std::vector<std::string>{"heartbeat-gap claim:1 warning", "heartbeat-gap claim:1 info", "heartbeat-gap claim:1 info"});
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

const std::vector<std::string> k_dispatch_checks{"dispatch-no-role-action", "dispatch-unconfirmed", "dispatch-confirmed-late",
                                                 "action-unended"};

TEST_CASE("a previewed and confirmed dispatch that completes with no role action is one finding",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  world w;
  w.seed_routing();
  // The 7335 shape: the dispatch record binds the claim, the claim completes, no coder action ever ran.
  auto token   = w.claim(1);
  auto preview = w.dispatch_preview(1, token);
  w.dispatch_confirm(preview, world::now_text(), token);
  CHECK(w.findings(k_dispatch_checks).empty()); // the claim is live and may still start its action
  w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});

  auto        parsed = w.diagnose_json(k_dispatch_checks);
  const auto& found  = parsed.find("findings")->array;
  REQUIRE(found.size() == 1);
  CHECK(found[0].find("check")->string == "dispatch-no-role-action");
  CHECK(found[0].find("entity")->string == "claim:1");
  CHECK(found[0].find("severity")->string == "warning");
  std::vector<std::string> evidence;
  for (const auto& e : found[0].find("evidence")->array) {
    evidence.push_back(e.string);
  }
  CHECK(std::ranges::is_permutation(evidence,
                                    std::vector<std::string>{"claim:1", "task:1", "dispatch_preview:1", "dispatch_snapshot:1"}));
}

TEST_CASE("a dispatch with a task-tied coder action under its claim produces no finding",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  world w;
  w.seed_routing();
  auto token   = w.claim(1);
  auto preview = w.dispatch_preview(1, token);
  w.dispatch_confirm(preview, world::now_text(), token);
  w.planar_agent({"action", "start", "--claim", token, "--kind", "coder", "--entity", "task:1", "--no-locality-probe"});
  w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});
  CHECK(w.findings(k_dispatch_checks).empty());
}

TEST_CASE("a direct claim doing its own work yields no dispatch finding, with or without a coder action",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  {
    world w;
    auto  token = w.claim(1);
    w.planar_agent({"action", "start", "--claim", token, "--kind", "coder", "--entity", "task:1", "--no-locality-probe"});
    w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});
    CHECK(w.findings(k_dispatch_checks).empty());
  }
  {
    world w;
    auto  token = w.claim(1);
    w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});
    CHECK(w.findings(k_dispatch_checks).empty());
  }
}

TEST_CASE("an action left open under a lapsed claim is reported, and not before the lease runs out",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  world w;
  auto  token = w.claim(1);
  w.planar_agent({"action", "start", "--claim", token, "--kind", "tool_call", "--entity", "task:1", "--no-locality-probe"});
  CHECK(w.findings({"action-unended"}).empty());

  // TIME TRAVEL: the lease lapsed long ago, with no terminal verb.
  w.move_time("update agent_work_claims set lease_expires_at = '2020-01-01T00:10:00.000Z'");
  auto        parsed = w.diagnose_json({"action-unended"});
  const auto& found  = parsed.find("findings")->array;
  // The claim's own claim_check action is open too; both are named, each with the claim.
  REQUIRE(found.size() == 2);
  std::vector<std::string> entities;
  for (const auto& f : found) {
    CHECK(f.find("check")->string == "action-unended");
    CHECK(f.find("severity")->string == "warning");
    entities.push_back(f.find("entity")->string);
    bool names_claim = false;
    for (const auto& e : f.find("evidence")->array) {
      names_claim = names_claim || e.string == "claim:1";
    }
    CHECK(names_claim);
  }
  CHECK(std::ranges::is_permutation(entities, std::vector<std::string>{"action:1", "action:2"}));

  // Reconcile marks the claim stale and closes only its claim_check marker, so the tool call is
  // still an action left open under an ended claim.
  w.planar_agent({"reconcile"});
  CHECK(w.findings({"action-unended"}) == std::vector<std::string>{"action-unended action:2 warning"});
}

TEST_CASE("previewed dispatches that were never confirmed are reported, a direct claim beside them is not",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  world w;
  w.seed_routing();
  w.planar({"task", "add", "Third task", "--plan", "1"});
  w.planar({"task", "add", "Fourth task", "--plan", "1"});
  // Tasks 1 and 2 are confirmed before their coder action starts; task 3 never is. Task 4 is
  // claimed directly with no preview.
  std::vector<std::string> tokens;
  for (int task = 1; task <= 3; ++task) {
    tokens.push_back(w.claim(task));
    auto preview = w.dispatch_preview(task, tokens.back());
    if (task != 3) {
      w.dispatch_confirm(preview, world::now_text(), tokens.back());
    }
    w.planar_agent({"action", "start", "--claim", tokens.back(), "--kind", "coder", "--entity", std::format("task:{}", task),
                    "--no-locality-probe"});
  }
  auto direct = w.claim(4);
  w.planar_agent({"action", "start", "--claim", direct, "--kind", "coder", "--entity", "task:4", "--no-locality-probe"});

  CHECK(w.findings(k_dispatch_checks) == std::vector<std::string>{"dispatch-unconfirmed claim:3 warning"});
}

TEST_CASE("a snapshot confirmed after the role action started is late, and not unconfirmed",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  world w;
  w.seed_routing();
  auto token   = w.claim(1);
  auto preview = w.dispatch_preview(1, token);
  w.planar_agent({"action", "start", "--claim", token, "--kind", "coder", "--entity", "task:1", "--no-locality-probe"});
  auto confirmed = world::now_text();
  w.dispatch_confirm(preview, confirmed, token);

  // TIME TRAVEL: a snapshot is immutable and stamped with the caller's whole-second text, and the
  // verbs write the real clock for the action, so the order of the two is not under the test's
  // control. The action is moved to 2020-01-01, long before the confirm.
  w.move_time("update agent_actions set started_at = '2020-01-01T00:00:00.000Z' where action_kind = 'coder'");
  auto        parsed = w.diagnose_json(k_dispatch_checks, "36500");
  const auto& found  = parsed.find("findings")->array;
  REQUIRE(found.size() == 1);
  CHECK(found[0].find("check")->string == "dispatch-confirmed-late");
  CHECK(found[0].find("entity")->string == "claim:1");
  REQUIRE(found[0].find("evidence_times")->array.size() == 2);
  CHECK(found[0].find("evidence_times")->array[0].string == "2020-01-01T00:00:00.000Z");
  // The snapshot's confirmed_at, normalised to the stored format.
  auto normalised = confirmed.substr(0, confirmed.size() - 1) + ".000Z";
  CHECK(found[0].find("evidence_times")->array[1].string == normalised);

  // The action starting at the instant of the confirm is not late.
  w.move_time(std::format("update agent_actions set started_at = '{}' where action_kind = 'coder'", normalised));
  CHECK(w.findings(k_dispatch_checks, "36500").empty());
}

TEST_CASE("an ingest apply with no preview before it is reported, and one after a preview is not",
          "[cmd][watch][diagnose][workflow][cli]") {
  {
    world w;
    w.enable_cli_log();
    CHECK(w.planar_status({"spec", "ingest", "1", "--apply"}) != 0);
    auto parsed = w.diagnose_any({"apply-without-preview"});
    CHECK(parsed.find("outcome")->string == "ok");
    const auto& found = parsed.find("findings")->array;
    REQUIRE(found.size() == 1);
    CHECK(found[0].find("check")->string == "apply-without-preview");
    CHECK(found[0].find("severity")->string == "warning");
    // The finding names the logged apply.
    auto apply_id = w.scalar("select id from cli_invocations where verb_path = 'spec ingest'");
    CHECK(found[0].find("entity")->string == std::format("cli_invocation:{}", apply_id));
  }
  {
    world w;
    w.enable_cli_log();
    CHECK(w.planar_status({"spec", "ingest", "1"}) != 0);
    CHECK(w.planar_status({"spec", "ingest", "1", "--apply"}) != 0);
    CHECK(w.scalar("select count(*) from cli_invocations where verb_path = 'spec ingest'") == 2);
    auto parsed = w.diagnose_any({"apply-without-preview"});
    CHECK(parsed.find("outcome")->string == "ok");
    CHECK(parsed.find("findings")->array.empty());
  }
}

TEST_CASE("with the capture log off the apply check is disabled and the outcome stays ok",
          "[cmd][watch][diagnose][workflow][cli]") {
  world w;
  // The default config leaves cli_log off, so the apply is not logged at all.
  CHECK(w.planar_status({"spec", "ingest", "1", "--apply"}) != 0);
  auto parsed = w.diagnose_any({"apply-without-preview"});
  CHECK(parsed.find("outcome")->string == "ok");
  CHECK(parsed.find("findings")->array.empty());
  bool seen = false;
  for (const auto& row : parsed.find("coverage")->array) {
    if (row.find("input")->string == "cli_log") {
      seen = true;
      CHECK(row.find("state")->string == "disabled");
      CHECK(row.find("reason")->string == "cli_log-off");
    }
  }
  CHECK(seen);

  // The same holds for a run that selects every check: a host that does not log still reads `ok`.
  CHECK(w.diagnose_any({}).find("outcome")->string == "ok");
}

TEST_CASE("a config file that cannot be parsed makes the capture-log input unavailable and the run partial",
          "[cmd][watch][diagnose][workflow][cli]") {
  world w;
  {
    std::ofstream config{w.root / "config.toml", std::ios::binary};
    config << "[introspection\ncli_log = \n";
    REQUIRE(config.good());
  }
  auto parsed = w.diagnose_any({"apply-without-preview"});
  CHECK(parsed.find("outcome")->string == "partial");
  CHECK(parsed.find("findings")->array.empty());
  bool seen = false;
  for (const auto& row : parsed.find("coverage")->array) {
    if (row.find("input")->string == "cli_log") {
      seen = true;
      CHECK(row.find("state")->string == "unavailable");
      CHECK(row.find("reason")->string == "cli_log-config-unknown");
    }
  }
  CHECK(seen);
}

TEST_CASE("a dispatch previewed and confirmed without a claim token, then claimed, is still that claim's dispatch",
          "[cmd][watch][diagnose][workflow][dispatch]") {
  // Real orchestrator runs preview with no claim token, confirm, and only then claim. The snapshot is
  // confirmed (the caller's text, a minute in the past here) before the claim row exists.
  {
    world w;
    w.seed_routing();
    auto preview = w.dispatch_preview(1, "");
    w.dispatch_confirm(preview, world::now_text(-60));
    auto token = w.claim(1);
    w.planar_agent({"complete", "--claim", token, "--no-locality-probe"});
    // The claim is a dispatch through the spent preview, and it ended with no role action.
    CHECK(w.findings(k_dispatch_checks) == std::vector<std::string>{"dispatch-no-role-action claim:1 warning"});
  }
  {
    world w;
    w.seed_routing();
    auto first = w.dispatch_preview(1, "");
    w.dispatch_confirm(first, world::now_text(-60));
    auto token = w.claim(1);
    w.planar_agent({"action", "start", "--claim", token, "--kind", "coder", "--entity", "task:1", "--no-locality-probe"});
    // A re-dispatch, bound to the claim, is confirmed after the coder action started.
    auto second = w.dispatch_preview(1, token);
    w.dispatch_confirm(second, world::now_text(), token);
    // TIME TRAVEL: the coder action started between the two confirms.
    w.move_time(std::format("update agent_actions set started_at = '{}.000Z' where action_kind = 'coder'",
                            world::now_text(-30).substr(0, 19)));
    // The first confirm came before the action, so the dispatch was not confirmed late.
    CHECK(w.findings(k_dispatch_checks).empty());
  }
}

TEST_CASE("three failing task adds form one cli-failure-cluster naming the verb and category",
          "[cmd][watch][diagnose][workflow][cluster]") {
  {
    world w;
    w.enable_cli_log();
    for (int i = 0; i < 3; ++i) {
      CHECK(w.planar_status({"task", "add"}) != 0);
    }
    CHECK(w.scalar("select count(*) from cli_invocations where verb_path = 'task add' and error_category = 'usage'") == 3);
    auto parsed = w.diagnose_any({"cli-failure-cluster"});
    CHECK(parsed.find("outcome")->string == "ok");
    const auto& found = parsed.find("findings")->array;
    REQUIRE(found.size() == 1);
    CHECK(found[0].find("check")->string == "cli-failure-cluster");
    CHECK(found[0].find("severity")->string == "warning");
    CHECK(found[0].find("fingerprint")->string == "cli-failure-cluster|task add|usage|global");
    CHECK(found[0].find("evidence")->array.size() == 3);
  }
  {
    // Two failures are below the threshold.
    world w;
    w.enable_cli_log();
    for (int i = 0; i < 2; ++i) {
      CHECK(w.planar_status({"task", "add"}) != 0);
    }
    CHECK(w.diagnose_any({"cli-failure-cluster"}).find("findings")->array.empty());
  }
  {
    // Failures of two verbs do not add up.
    world w;
    w.enable_cli_log();
    CHECK(w.planar_status({"task", "add"}) != 0);
    CHECK(w.planar_status({"task", "add"}) != 0);
    CHECK(w.planar_status({"task", "show", "99999"}) != 0);
    CHECK(w.diagnose_any({"cli-failure-cluster"}).find("findings")->array.empty());
  }
}

TEST_CASE("with the capture log off the cluster check is disabled and reports nothing",
          "[cmd][watch][diagnose][workflow][cluster]") {
  world w;
  for (int i = 0; i < 3; ++i) {
    CHECK(w.planar_status({"task", "add"}) != 0);
  }
  auto parsed = w.diagnose_any({"cli-failure-cluster"});
  CHECK(parsed.find("outcome")->string == "ok");
  CHECK(parsed.find("findings")->array.empty());
}

TEST_CASE("claims ended with planar-agent fail --category form a claim-failure-cluster, unknown ones do not",
          "[cmd][watch][diagnose][workflow][cluster]") {
  {
    world w;
    w.planar({"task", "add", "Third task", "--plan", "1"});
    for (int task = 1; task <= 3; ++task) {
      auto token = w.claim(task);
      w.planar_agent({"fail", "--claim", token, "--reason", "tool crashed", "--category", "tool_failure", "--no-locality-probe"});
    }
    auto parsed = w.diagnose_any({"claim-failure-cluster"});
    CHECK(parsed.find("outcome")->string == "ok");
    const auto& found = parsed.find("findings")->array;
    REQUIRE(found.size() == 1);
    CHECK(found[0].find("check")->string == "claim-failure-cluster");
    CHECK(found[0].find("severity")->string == "warning");
    CHECK(found[0].find("fingerprint")->string.starts_with("claim-failure-cluster|failure_category=tool_failure|"));
    CHECK(found[0].find("evidence")->array.size() == 3);
  }
  {
    // `fail` without a category leaves the claim `unknown`, which is no classification.
    world w;
    w.planar({"task", "add", "Third task", "--plan", "1"});
    for (int task = 1; task <= 3; ++task) {
      auto token = w.claim(task);
      w.planar_agent({"fail", "--claim", token, "--reason", "tool crashed", "--no-locality-probe"});
    }
    CHECK(w.diagnose_any({"claim-failure-cluster"}).find("findings")->array.empty());
  }
}

namespace {

/// A Jira issue body for `DEMO-1` with the given summary and version marker.
auto jira_issue(std::string_view summary, std::string_view updated) -> std::string {
  return std::format(R"({{"key":"DEMO-1","fields":{{"summary":"{}","status":{{"name":"To Do"}},"updated":"{}"}}}})", summary,
                     updated);
}

} // namespace

TEST_CASE("a sync conflict through planar-ext is reported until planar-ext sync resolve settles it",
          "[cmd][watch][diagnose][workflow][sync]") {
  std::mutex                    guard;
  std::string                   summary = "First task";
  std::string                   updated = "2026-01-01T00:00:00.000+0000";
  planar::http::fixture::server fake_jira([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const lock{guard};
    if (req.verb == "PUT") {
      return planar::http::fixture::canned_response{.status = 204, .body = {}, .content_type = "application/json"};
    }
    return planar::http::fixture::canned_response{
        .status = 200, .body = jira_issue(summary, updated), .content_type = "application/json"};
  });
  auto                          remote_changes = [&](std::string_view new_summary, std::string_view new_updated) {
    std::scoped_lock const lock{guard};
    summary = new_summary;
    updated = new_updated;
  };

  world w;
  REQUIRE(w.ext({"ext", "register", "jira", "jira-demo", "--base-url", fake_jira.base_url(), "--project", "DEMO", "--auth-env",
                 "DEMO_TOKEN"})
              .code == 0);
  w.planar({"link", "task:1", "--to", "jira-demo:DEMO-1", "--role", "mirror", "--sync", "two-way"});

  // Nothing to report before any sync, and after clean ones.
  CHECK(w.findings({"sync-conflict-unresolved"}).empty());
  CHECK(w.ext({"sync", "pull", "1"}).code == 0);
  remote_changes("Renamed remotely", "2026-02-02T00:00:00.000+0000");
  CHECK(w.ext({"sync", "pull", "1"}).code == 0);
  CHECK(w.findings({"sync-conflict-unresolved"}).empty());

  // Both sides change: the pull records a conflict and exits 3.
  w.planar({"task", "update", "1", "--title", "Renamed locally"});
  remote_changes("Renamed remotely again", "2026-03-03T00:00:00.000+0000");
  CHECK(w.ext({"sync", "pull", "1"}).code == 3);
  auto event = w.scalar("select max(id) from sync_events where outcome = 'conflict'");
  auto found = w.diagnose_json({"sync-conflict-unresolved"});
  REQUIRE(found.find("findings")->array.size() == 1);
  const auto& finding = found.find("findings")->array[0];
  CHECK(finding.find("check")->string == "sync-conflict-unresolved");
  CHECK(finding.find("severity")->string == "warning");
  CHECK(finding.find("entity")->string == std::format("sync_event:{}", event));
  CHECK(finding.find("fingerprint")->string == "sync-conflict-unresolved|external_link:1|global");
  CHECK(finding.find("evidence")->array.size() == 2);

  // Settle it the way an operator does: the token from the event's evidence, the task's current version.
  auto token = w.text(std::format("select json_extract(context_json, '$.token') from sync_events where id = {}", event));
  auto local = w.text("select updated_at from tasks where id = 1");
  REQUIRE(!token.empty());
  auto resolved = w.ext({"sync", "resolve", std::to_string(event), "--keep", "local", "--evidence-token", token,
                         "--expected-local-updated-at", local});
  CHECK(resolved.code == 0);
  CHECK(w.findings({"sync-conflict-unresolved"}).empty());

  // A second conflict on the same link is the same incident: one fingerprint, with the new event as its primary entity.
  w.planar({"task", "update", "1", "--title", "Renamed locally again"});
  remote_changes("Renamed remotely a third time", "2026-04-04T00:00:00.000+0000");
  CHECK(w.ext({"sync", "pull", "1"}).code == 3);
  auto again = w.scalar("select max(id) from sync_events where outcome = 'conflict'");
  CHECK(again > event);
  auto second = w.diagnose_json({"sync-conflict-unresolved"});
  REQUIRE(second.find("findings")->array.size() == 1);
  CHECK(second.find("findings")->array[0].find("entity")->string == std::format("sync_event:{}", again));
  CHECK(second.find("findings")->array[0].find("fingerprint")->string == finding.find("fingerprint")->string);

  // Pulling again while the link still conflicts writes yet another event: still one finding, still one fingerprint.
  CHECK(w.ext({"sync", "pull", "1"}).code == 3);
  auto third = w.diagnose_json({"sync-conflict-unresolved"});
  REQUIRE(third.find("findings")->array.size() == 1);
  CHECK(third.find("findings")->array[0].find("fingerprint")->string == finding.find("fingerprint")->string);
}
