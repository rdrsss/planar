// @file diagnose_workflow.t.cpp
// @brief Diagnose workflow scenarios through the real binaries, end to end (plan 1132, task 7385).
//
// Each case builds a scratch database with `planar` (anchor plan, milestone plan, tasks) and
// `planar-agent` (claim, dispatch preview and confirm, action, heartbeat, complete), then reads the
// same findings from the three places an operator meets them: `planar-watch diagnose --json` on the
// anchor and on the milestone, the `planar-agent complete` that promotes the milestone to `done`
// (its `diagnose` key), and `planar plan closeout --dry-run --json` on the anchor. The three must
// agree, exactly:
//
//   * the 7335 shape: a dispatch previewed and confirmed whose claim completes with no coder
//     action is one `dispatch-no-role-action` warning;
//   * the 7337 shape: a lapsed claim left active behind a recovery claim that finished the task
//     is a `claim-superseded-active` error, with its open claim_check marker as an `action-unended` warning;
//   * an action left open under a claim that lapsed and was reconciled is one `action-unended` warning beside the info event
//   `claim-closed-by-reconcile`;
//   * a direct claim doing its own work, and a healthy orchestrated run, give no finding.
//
// The one write outside a verb is TIME TRAVEL: a lease cannot be waited out, so a lease timestamp
// is moved afterwards. Dispatch previews need a routing candidate no verb creates; that row is the
// only one inserted by hand.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.json_dom;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;
namespace jd     = planar::json_dom;

struct world {
  parity::arena         arena = parity::make_arena("watch_diagnose_workflow");
  std::filesystem::path root  = arena.cpp_root;
  std::filesystem::path op    = std::filesystem::path{PLANAR_OPERATOR_CPP_BIN};
  std::filesystem::path agent = std::filesystem::path{PLANAR_AGENT_CPP_BIN};
  std::filesystem::path watch = std::filesystem::path{PLANAR_CPP_BIN};
  int                   seq   = 0;

  auto tag() -> std::string {
    return std::format("w{}", ++seq);
  }

  auto run(std::filesystem::path const& bin, std::vector<std::string> args) -> parity::capture {
    auto got = parity::run_pinned(bin, args, root, tag());
    INFO(bin.filename().string() << " " << args.front() << ": " << got.err << got.out);
    REQUIRE(got.code == 0);
    return got;
  }

  auto planar(std::vector<std::string> args) -> parity::capture {
    return run(op, std::move(args));
  }

  auto planar_agent(std::vector<std::string> args) -> parity::capture {
    return run(agent, std::move(args));
  }

  /// Anchor plan 1 with milestone plan 2 holding `tasks` tasks (ids 1..), the anchor active.
  explicit world(int tasks) {
    planar({"init"});
    planar({"assoc", "create", "project:proj", "--kind", "project"});
    planar({"assoc", "add", "project:proj", (root / "proj").string()});
    planar({"plan", "create", "Anchor plan"});
    planar({"plan", "create", "Milestone one", "--parent", "1"});
    for (int i = 1; i <= tasks; ++i) {
      planar({"task", "add", std::format("Task {}", i), "--plan", "2"});
    }
    planar({"plan", "update", "1", "--status", "active"});
    sql("insert into routing_candidates (vendor, candidate_id, fallback_order) values ('claude', 'candidate', 0)");
  }

  /// TIME TRAVEL and the routing candidate: the only writes made outside a verb.
  auto sql(std::string_view statement) -> void {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    INFO(statement);
    REQUIRE(conn->execute(statement).has_value());
  }

  auto text(std::string_view query) -> std::string {
    auto conn = planar::db::connection::open((root / "planar.db").string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(query);
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    return stmt->column_text(0);
  }

  /// Claims a task through the real verb and returns the claim token.
  auto claim(int task_id, bool no_transition = false) -> std::string {
    std::vector<std::string> args{
        "claim", "--entity",           std::format("task:{}", task_id), "--vendor", "claude", "--role", "coder", "--ttl",
        "30m",   "--no-locality-probe"};
    if (no_transition) {
      args.push_back("--no-transition");
    }
    auto const got = planar_agent(args);
    auto const at  = got.out.find("claim:");
    REQUIRE(at != std::string::npos);
    return got.out.substr(at + 6, got.out.find(' ', at + 6) - (at + 6));
  }

  auto start_action(std::string const& token, std::string const& kind, int task_id) -> void {
    planar_agent({"action", "start", "--claim", token, "--kind", kind, "--entity", std::format("task:{}", task_id),
                  "--no-locality-probe"});
  }

  static auto now_text(int offset_seconds = 0) -> std::string {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() +
                                                                                         std::chrono::seconds{offset_seconds}));
  }

  /// The orchestrator's dispatch, in its order: preview (no claim yet) then confirm. Synthetic digests, consistent.
  auto dispatch(int task_id) -> void {
    auto const got = planar_agent({"dispatch",
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
                                   "2099-01-01T00:00:00Z"});
    auto const at  = got.out.find("preview:");
    REQUIRE(at != std::string::npos);
    auto const start = got.out.find_first_not_of(' ', at + 8);
    auto const token = got.out.substr(start, got.out.find_first_of(" \n", start) - start);
    planar_agent({"dispatch",
                  "confirm",
                  "--token",
                  token,
                  "--dispatch-key",
                  "key-" + token,
                  "--now",
                  now_text(-60),
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
                  "r1"});
  }

  /// `check entity severity` for each finding in a diagnose result object, sorted.
  static auto summarize(jd::json_value const& diagnose) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (auto const& f : diagnose.find("findings")->array) {
      out.push_back(std::format("{} {} {}", f.find("check")->string, f.find("entity")->string, f.find("severity")->string));
    }
    std::ranges::sort(out);
    return out;
  }

  auto parse(std::string const& text_out) -> jd::json_value {
    auto parsed = jd::parse_json(text_out);
    INFO(text_out);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
  }

  /// `planar-watch diagnose --plan <id> --json`, outcome required to be ok.
  auto watch_findings(int plan_id) -> std::vector<std::string> {
    auto const got    = run(watch, {"diagnose", "--plan", std::to_string(plan_id), "--json"});
    auto const parsed = parse(got.out);
    CHECK(parsed.find("outcome")->string == "ok");
    return summarize(parsed);
  }

  /// `planar-agent complete --json`; the findings in its `diagnose` key, or nullopt when it has none.
  auto complete_findings(std::string const& token) -> std::optional<std::vector<std::string>> {
    auto const  got    = planar_agent({"complete", "--claim", token, "--no-locality-probe", "--json"});
    auto const  parsed = parse(got.out);
    auto const* key    = parsed.find("diagnose");
    if (key == nullptr) {
      return std::nullopt;
    }
    CHECK(key->find("outcome")->string == "ok");
    return summarize(*key);
  }

  /// `planar plan closeout 1 --dry-run --json`: the gate verdict and the findings in its `diagnose` key.
  auto closeout_findings() -> std::vector<std::string> {
    auto const got    = planar({"plan", "closeout", "1", "--dry-run", "--json"});
    auto const parsed = parse(got.out);
    CHECK(got.out.find(R"("ready":true)") != std::string::npos);
    auto const* key = parsed.find("diagnose");
    REQUIRE(key != nullptr);
    CHECK(key->find("outcome")->string == "ok");
    return summarize(*key);
  }
};

using findings = std::vector<std::string>;

} // namespace

TEST_CASE("the 7335 shape: a confirmed dispatch whose claim completes with no coder action is one warning",
          "[cmd][watch][diagnose][workflow][integration][7385]") {
  world w{1};
  w.dispatch(1);
  auto const token = w.claim(1);
  // The claim is live and may still start its action.
  CHECK(w.watch_findings(1).empty());

  auto const at_complete = w.complete_findings(token);
  REQUIRE(at_complete.has_value());
  CHECK(w.text("select status from plans where id = 2") == "done");
  findings const expected{"dispatch-no-role-action claim:1 warning"};
  CHECK(*at_complete == expected);
  CHECK(w.watch_findings(1) == expected);
  CHECK(w.watch_findings(2) == expected);
  CHECK(w.closeout_findings() == expected);
}

TEST_CASE("the 7337 shape: a lapsed claim left active behind the claim that finished the task is an error and its open marker",
          "[cmd][watch][diagnose][workflow][integration][7385]") {
  world      w{1};
  auto const first = w.claim(1);
  w.planar_agent({"heartbeat", "--claim", first});
  // TIME TRAVEL: the first claim's lease lapsed; recovery claims with --no-transition (--force would mark it stale).
  w.sql("update agent_work_claims set lease_expires_at = '2020-01-01T00:10:00.000Z'");
  auto const recovery = w.claim(1, true);

  auto const at_complete = w.complete_findings(recovery);
  REQUIRE(at_complete.has_value());
  CHECK(w.text("select status from plans where id = 2") == "done");
  // The stranded claim's own claim_check marker (action 1) is still open under its lapsed lease: the same
  // stranding, reported as the warning beside the error.
  findings const expected{"action-unended action:1 warning", "claim-superseded-active claim:1 error"};
  CHECK(*at_complete == expected);
  CHECK(w.watch_findings(1) == expected);
  CHECK(w.watch_findings(2) == expected);
  CHECK(w.closeout_findings() == expected);
}

TEST_CASE("an action left open on a claim that lapsed and was reconciled is one warning",
          "[cmd][watch][diagnose][workflow][integration][7385]") {
  world      w{1};
  auto const lapsed = w.claim(1);
  w.start_action(lapsed, "tool_call", 1);
  // TIME TRAVEL: the lease lapses with the action still open (the action started just now, inside the window).
  w.sql("update agent_work_claims set lease_expires_at = '2020-01-01T00:10:00.000Z'");
  w.planar_agent({"reconcile"});
  CHECK(w.text("select status from tasks where id = 1") == "todo");

  auto const retry       = w.claim(1);
  auto const at_complete = w.complete_findings(retry);
  REQUIRE(at_complete.has_value());
  CHECK(w.text("select status from plans where id = 2") == "done");
  // The reconcile leaves the info event beside the open tool call.
  findings const expected{"action-unended action:2 warning", "claim-closed-by-reconcile claim:1 info"};
  CHECK(*at_complete == expected);
  CHECK(w.watch_findings(1) == expected);
  CHECK(w.watch_findings(2) == expected);
  CHECK(w.closeout_findings() == expected);
}

TEST_CASE("a direct claim doing its own work gives no dispatch finding anywhere",
          "[cmd][watch][diagnose][workflow][integration][7385]") {
  world w{2};
  // Task 1 with a coder action, task 2 with none: neither is a dispatch.
  auto const first = w.claim(1);
  w.start_action(first, "coder", 1);
  w.planar_agent({"heartbeat", "--claim", first});
  auto const mid = w.complete_findings(first);
  CHECK_FALSE(mid.has_value()); // the milestone is not promoted yet
  auto const second      = w.claim(2);
  auto const at_complete = w.complete_findings(second);
  REQUIRE(at_complete.has_value());
  CHECK(w.text("select status from plans where id = 2") == "done");
  CHECK(at_complete->empty());
  CHECK(w.watch_findings(1).empty());
  CHECK(w.watch_findings(2).empty());
  CHECK(w.closeout_findings().empty());
}

TEST_CASE("a healthy orchestrated run, preview to complete, gives no finding anywhere",
          "[cmd][watch][diagnose][workflow][integration][7385]") {
  world w{1};
  w.dispatch(1);
  auto const token = w.claim(1);
  w.start_action(token, "coder", 1);
  w.planar_agent({"heartbeat", "--claim", token});
  CHECK(w.watch_findings(1).empty());

  auto const at_complete = w.complete_findings(token);
  REQUIRE(at_complete.has_value());
  CHECK(w.text("select status from plans where id = 2") == "done");
  CHECK(at_complete->empty());
  CHECK(w.watch_findings(1).empty());
  CHECK(w.watch_findings(2).empty());
  CHECK(w.closeout_findings().empty());
}
