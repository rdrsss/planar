// @file plan_closeout_leaf.t.cpp
// @brief In-process tests for the leaf plan 996 task 6317 landed:
// `planar plan closeout`.
//
// ## THIS LEAF WRITES, AND `CLAUDE.md` CALLS IT AUTHORITATIVE
//
// It is the gate the janitor runs to close a plan: without `--dry-run` it
// sets `plans.status = 'done'` and writes an `audit_log` row. Every capture
// behind this file was taken in a pinned scratch arena (`PLANAR_DB` under a
// temp root) against fixture plans created for the purpose, never against
// the operator's database. Each case here dispatches in-process against its
// own fixture for the same reason.
//
// ## THE FIRST CASE ASSERTS THE FIXTURE
//
// A closeout report of all zeros is the CORRECT answer for an empty plan,
// so a fixture whose seeds silently failed would satisfy most of the
// comparisons below while exercising one arm of five. That is not
// hypothetical on this verb: the oracle probe written alongside this file
// hit `task done <id>` from `todo` — `IllegalTransition`, exit 1 — on its
// first run, and every task stayed `todo`. `seed` therefore REQUIREs each
// step's exit code, and the first case pins the resulting shape before any
// byte comparison runs.
//
// ## THE FIVE HARD-GATE ARMS ARE EACH PAIRED WITH THEIR ABSENCE
//
// open task / open descendant plan / live claim each have a firing case AND
// a case on the same database where the rule does not fire; `cancelled`
// tasks and stale claims have the reverse (they are the pair that proves
// the counted-but-not-blocking half). An assertion that `blocked_by` is
// empty can pass because the fixture matched nothing.
//
// ## ORACLE PROVENANCE
//
// Captured from `zig/zig-out/bin/planar` built at this cycle's base. Exit
// codes were read from the command itself with stdout and stderr redirected
// to SEPARATE files — not through a pipe, and not into one shared file: the
// first attempt used `>f 2>&1` and the stderr line overwrote the head of
// the report, which would have been read as the oracle suppressing stdout
// on the blocked-apply path when in fact it emits both.
//
// The whole capture was then replayed as a 23-arm differential against the
// built C++ binary in a second identically-seeded arena. All 23 agreed on
// stdout, stderr, exit code, and every asserted row of `plans`, `tasks`,
// `agent_work_claims` and `audit_log`.
//
// The captures that decided a shape:
//
//   $Z plan closeout 1 --dry-run     -> exit 0 EVEN WHEN BLOCKED. The verb's
//       own `--help` says the opposite ("non-zero exit in both dry-run and
//       apply modes"); that sentence is stale in the oracle. Only the APPLY
//       path refuses, at exit 3.
//
//   $Z plan closeout 2               -> on the blocked APPLY path the FULL
//       report still goes to stdout and the refusal to stderr. Both.
//
//   $Z plan closeout 1 --dry-run     -> on a `draft` plan with no tasks the
//       banner reads "ready (already terminal — no change)" while the plan
//       is neither terminal nor changed. Oracle defect, reproduced.
//
//   $Z plan closeout 999             -> exit 1, `no plan with id 999`. NOT
//       `plan next`'s wording for the same condition (`plan 999 not found`);
//       the two were captured separately rather than assuming the sibling
//       transferred.
//
//   $Z plan closeout 1 --json        -> after the plan is closed,
//       `git_evidence` is `[]`. Before it was closed, on the same database,
//       it was a ONE-entry `(none)` synthetic. The already-terminal
//       short-circuit returns a different shape, and both were captured on
//       the same plan.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_closeout_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table, output untouched.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch_raw(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief The report as it was before the diagnose section (plan 1132, task 7384): the text before
/// `diagnose:`'s own line, or the JSON object without its trailing `diagnose` member. Output with no
/// section comes back unchanged, so a case that wants the section uses `dispatch_raw`.
/// @param out Everything the verb wrote to stdout.
/// @return The report without its diagnose section.
auto without_diagnose(std::string out) -> std::string {
  if (auto const json = out.find(R"(,"diagnose":{)"); json != std::string::npos) {
    return out.substr(0, json) + "}\n";
  }
  if (auto const line = out.find("\ndiagnose: "); line != std::string::npos) {
    // The section follows the report's own final newline and one separating newline.
    return out.substr(0, line);
  }
  return out;
}

/// @brief Dispatch `args` and drop the diagnose section from stdout, so a case about the gate report
/// compares the report alone. `dispatch_raw` keeps the section.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation with the section removed.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  auto result = dispatch_raw(fx, std::move(args));
  result.out  = without_diagnose(std::move(result.out));
  return result;
}

/// @brief Read one integer out of the fixture database.
auto scalar(const fixture& fx, std::string_view sql) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Read one string out of the fixture database.
auto text(const fixture& fx, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// @brief Run raw SQL against the fixture database.
///
/// Used ONLY to seed `agent_work_claims`: those rows are written by
/// `planar-agent`, which this binary's dispatch table cannot reach, and
/// there is no `planar` verb that creates one.
auto raw_sql(const fixture& fx, std::string_view sql) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto ok = conn->execute(sql);
  REQUIRE(ok.has_value());
}

/// @brief Seed the shared fixture, asserting every step's exit code.
///
/// The shape, and why each piece is here:
///
///   plan 1  no tasks — the ready-and-empty case, and the plan the
///           already-terminal arm is measured on (before and after closing).
///   plan 2  one `todo` task — the open-task refusal, and the plan the
///           blocked-APPLY exit-3 case runs against.
///   plan 3  one `cancelled` task — terminal history: counted, not blocking.
///   plan 4  a `draft` CHILD (plan 5) and no tasks — the open-descendant
///           refusal in isolation from the task rule.
///   plan 6  one `done` task carrying a LIVE claim — the claim refusal with
///           every task already terminal, so the reason is unambiguous.
///   plan 7  three finalization-slugged tasks plus one ordinary, all `todo`
///           — the labelling count beside a blocking open-task count.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  for (auto const& [title, slug] : std::vector<std::pair<std::string, std::string>>{{"Empty ready plan", "pready"},
                                                                                    {"Open task plan", "popen"},
                                                                                    {"Cancelled only", "pcanc"},
                                                                                    {"Parent with child", "pparent"}}) {
    REQUIRE(dispatch(fx, {"plan", "create", title, "--scope", "global", "--slug", slug, "--json"}).code == 0);
  }
  REQUIRE(dispatch(fx, {"plan", "create", "Child", "--scope", "global", "--slug", "pchild", "--parent", "4", "--json"}).code ==
          0);
  REQUIRE(dispatch(fx, {"plan", "create", "Claims plan", "--scope", "global", "--slug", "pclaims", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Finalization slugs", "--scope", "global", "--slug", "pfinal", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "add", "Open one", "--plan", "2", "--scope", "global", "--no-editor", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "To cancel", "--plan", "3", "--scope", "global", "--no-editor", "--json"}).code == 0);
  // `task cancel` from `todo` is legal; `task done` from `todo` is NOT (see
  // this file's header), which is why the claimed task takes two steps.
  REQUIRE(dispatch(fx, {"task", "cancel", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Claimed one", "--plan", "6", "--scope", "global", "--no-editor", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "3", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "3"}).code == 0);

  for (auto const& [title, slug] : std::vector<std::pair<std::string, std::string>>{{"Finalize it", "finalize-thing"},
                                                                                    {"Merge it", "merge-thing"},
                                                                                    {"Reconcile it", "reconcile-thing"},
                                                                                    {"Unrelated", "plain-thing"}}) {
    REQUIRE(
        dispatch(fx, {"task", "add", title, "--plan", "7", "--slug", slug, "--scope", "global", "--no-editor", "--json"}).code ==
        0);
  }

  raw_sql(fx, "insert into sessions (vendor) values ('probe')");
  raw_sql(fx, "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, "
              "lease_expires_at) values ('live-1', 1, 'task', 3, 'active', 'probe', '2099-01-01T00:00:00.000Z')");
  raw_sql(fx, "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, "
              "lease_expires_at) values ('stale-1', 1, 'task', 1, 'active', 'probe', '2000-01-01T00:00:00.000Z')");
}

/// @brief The synthetic git-evidence entry, as it appears in JSON.
///
/// Raw literals here carry the `J` delimiter throughout: the payloads
/// contain `)"` — `"(none)"`, `blocked)"]`, `wired)"}]` — each of which
/// closes a bare `R"(` literal mid-string.
constexpr std::string_view k_none_evidence =
    R"J([{"repo_root":"(none)","branch":null,"target_branch":null,"base_merged":null,"branch_merged":null,)J"
    R"J("note":"no commit attribution — inconclusive (hardens once session-commit capture is wired)"}])J";

/// @brief The synthetic git-evidence entry, as it appears in text.
constexpr std::string_view k_none_evidence_text =
    "\ngit evidence (advisory):\n"
    "  repo: (none)\n"
    "    branch:  (none)  target: (unknown)\n"
    "    note:    no commit attribution — inconclusive (hardens once session-commit capture is wired)\n";

} // namespace

TEST_CASE("the closeout fixture has the shape every later case assumes", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("shape");
  seed(fx);

  CHECK(scalar(fx, "select count(*) from plans") == 7);
  CHECK(text(fx, "select status from plans where id=1") == "draft");
  CHECK(text(fx, "select status from plans where id=5") == "draft"); // the open CHILD of plan 4
  CHECK(scalar(fx, "select parent_plan_id from plans where id=5") == 4);

  // Plans 3 and 6 are `active`, NOT `draft`: transitioning their tasks ran
  // plan-304's auto-promotion. Both are still OPEN either way — `draft`,
  // `active` and `paused` are one bucket to this gate — but a later case
  // that hardcoded `draft` here would fail for an unrelated reason. Pinned
  // so the surprise is recorded rather than rediscovered.
  CHECK(text(fx, "select status from plans where id=3") == "active");
  CHECK(text(fx, "select status from plans where id=6") == "active");

  // The three task statuses the gate distinguishes, each actually present.
  CHECK(text(fx, "select status from tasks where id=1") == "todo");
  CHECK(text(fx, "select status from tasks where id=2") == "cancelled");
  CHECK(text(fx, "select status from tasks where id=3") == "done");
  CHECK(scalar(fx, "select count(*) from tasks where slug like 'finalize-%' or slug like 'merge-%' "
                   "or slug like 'reconcile-%'") == 3);

  // One live claim and one stale one — the pair the claim rule needs.
  CHECK(scalar(fx, "select count(*) from agent_work_claims where status='active'") == 2);
  CHECK(scalar(fx, "select count(*) from agent_work_claims where lease_expires_at > "
                   "strftime('%Y-%m-%dT%H:%M:%fZ','now')") == 1);

  // Nothing has been closed yet, so every later `status='done'` assertion
  // is about a change this file caused.
  CHECK(scalar(fx, "select count(*) from plans where status='done'") == 0);
  CHECK(scalar(fx, "select count(*) from audit_log where summary like 'closeout%'") == 0);
}

TEST_CASE("plan closeout refuses a missing plan at 1 and a non-integer id at 2", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("refuse");
  seed(fx);

  auto const missing = dispatch(fx, {"plan", "closeout", "999", "--json"});
  CHECK(missing.code == 1);
  CHECK(missing.out == planar::cmd::testsupport::json_error_envelope_line("plan closeout", "not_found"));
  CHECK(missing.err == "error: no plan with id 999\n");

  auto const bad = dispatch(fx, {"plan", "closeout", "abc", "--json"});
  CHECK(bad.code == 2);
  CHECK(bad.out == planar::cmd::testsupport::json_error_envelope_line("plan closeout", "invalid_input"));
  CHECK(bad.err == "error: plan id must be an integer, got 'abc'\n");

  // Neither refusal touched the database.
  CHECK(scalar(fx, "select count(*) from plans where status='done'") == 0);
}

TEST_CASE("a ready plan reports ready in both renderings", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("ready");
  seed(fx);

  auto const json = dispatch(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  CHECK(json.out == std::format(R"J({{"plan":1,"ready":true,"applied":false,"hard_evidence":{{)J"
                                R"J("tasks":{{"open":0,"done":0,"cancelled":0}},"descendants":{{"open":0,"terminal":0}},)J"
                                R"J("claims":{{"live":0,"stale":0}},"finalization_tasks":0}},"blocked_by":[],)J"
                                R"J("git_evidence":{},"epic_merge":null,"warnings":[]}}
)J",
                                k_none_evidence));

  // THE BANNER, corrected at task 6318. Plan 1 is `draft` and this call
  // changed nothing; the old wording claimed it was already terminal, which
  // is the opposite of what `--dry-run` is asked to report.
  auto const text_mode = dispatch(fx, {"plan", "closeout", "1", "--dry-run"});
  CHECK(text_mode.code == 0);
  CHECK(text_mode.out == std::format("[dry-run] plan 1: ready to close (no change made)\n"
                                     "\n"
                                     "hard gate:\n"
                                     "  tasks:       open=0  done=0  cancelled=0\n"
                                     "  descendants: open=0  terminal=0\n"
                                     "  claims:      live=0  stale=0\n"
                                     "{}",
                                     k_none_evidence_text));
  CHECK(text(fx, "select status from plans where id=1") == "draft");
}

// NOTE the leading word rather than a leading `--`. `catch_discover_tests`
// registers the TITLE as the ctest test name and ctest passes it back to the
// binary as an argument, so a title starting with `--` is parsed as a Catch2
// FLAG: green when run by tag, red under ctest, for no reason visible in the
// test. Two titles in this file would have had it.
TEST_CASE("dry-run exits 0 even when the gate fails, and apply exits 3", "[cmd][plan][closeout]") {
  // The pair the verb's own stale help text gets wrong. Both arms run
  // against the SAME plan so the only variable is the flag.
  auto const fx = make_fixture("exitcodes");
  seed(fx);

  auto const preview = dispatch(fx, {"plan", "closeout", "2", "--dry-run", "--json"});
  CHECK(preview.code == 0);
  CHECK(preview.err.empty());
  CHECK(preview.out.find(R"J("ready":false)J") != std::string::npos);
  CHECK(preview.out.find(R"J("blocked_by":["1 open task(s) on plan (todo/doing/blocked)"])J") != std::string::npos);

  auto const applied = dispatch(fx, {"plan", "closeout", "2"});
  CHECK(applied.code == 3);
  // Both streams, in this order. Losing the report would leave the operator
  // with a count and no account of what blocked.
  CHECK(applied.err == "error: plan 2 is not ready to close (1 reason(s))\n");
  CHECK(applied.out == std::format("plan 2: NOT ready to close\n"
                                   "\n"
                                   "hard gate:\n"
                                   "  tasks:       open=1  done=0  cancelled=0\n"
                                   "  descendants: open=0  terminal=0\n"
                                   "  claims:      live=0  stale=1\n"
                                   "\n"
                                   "blocked by:\n"
                                   "  - 1 open task(s) on plan (todo/doing/blocked)\n"
                                   "\n"
                                   "warnings:\n"
                                   "  ! 1 stale/expired claim(s) on plan tasks — reconcilable, not blocking\n"
                                   "{}",
                                   k_none_evidence_text));

  // The refused apply wrote nothing.
  CHECK(text(fx, "select status from plans where id=2") == "draft");
  CHECK(scalar(fx, "select count(*) from audit_log where summary like 'closeout%'") == 0);
}

TEST_CASE("cancelled tasks and stale claims are counted without blocking", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("terminalhistory");
  seed(fx);

  // Plan 3's only task is `cancelled`.
  auto const cancelled = dispatch(fx, {"plan", "closeout", "3", "--dry-run", "--json"});
  CHECK(cancelled.code == 0);
  CHECK(cancelled.out.find(R"J("ready":true)J") != std::string::npos);
  CHECK(cancelled.out.find(R"J("tasks":{"open":0,"done":0,"cancelled":1})J") != std::string::npos);
  CHECK(cancelled.out.find(R"J("blocked_by":[])J") != std::string::npos);

  // Plan 2 carries the stale claim: warned, NOT blocked BY IT (it is blocked
  // by its open task, which is a different reason — so `blocked_by` has
  // exactly one entry and it is not the claim one).
  auto const stale = dispatch(fx, {"plan", "closeout", "2", "--dry-run", "--json"});
  CHECK(stale.out.find(R"J("claims":{"live":0,"stale":1})J") != std::string::npos);
  CHECK(stale.out.find(R"J("warnings":["1 stale/expired claim(s) on plan tasks — reconcilable, not blocking"])J") !=
        std::string::npos);
  CHECK(stale.out.find("live claim(s) still active") == std::string::npos);
}

TEST_CASE("an open descendant plan blocks on its own", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("descendants");
  seed(fx);

  // Plan 4 has NO tasks, so the only rule that can fire is the descendant
  // one — which is why the fixture gave it a child and nothing else.
  auto const blocked = dispatch(fx, {"plan", "closeout", "4", "--dry-run", "--json"});
  CHECK(blocked.code == 0);
  CHECK(blocked.out.find(R"J("tasks":{"open":0,"done":0,"cancelled":0})J") != std::string::npos);
  CHECK(blocked.out.find(R"J("descendants":{"open":1,"terminal":0})J") != std::string::npos);
  CHECK(blocked.out.find(R"J("blocked_by":["1 open descendant plan(s) (draft/active/paused)"])J") != std::string::npos);

  // The paired non-firing case: the CHILD itself has no descendants.
  auto const leaf = dispatch(fx, {"plan", "closeout", "5", "--dry-run", "--json"});
  CHECK(leaf.out.find(R"J("descendants":{"open":0,"terminal":0})J") != std::string::npos);
  CHECK(leaf.out.find(R"J("ready":true)J") != std::string::npos);
}

TEST_CASE("a live claim blocks a plan whose tasks are all terminal", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("liveclaim");
  seed(fx);

  auto const blocked = dispatch(fx, {"plan", "closeout", "6", "--dry-run", "--json"});
  CHECK(blocked.code == 0);
  // Every task done — so the ONLY reason can be the claim.
  CHECK(blocked.out.find(R"J("tasks":{"open":0,"done":1,"cancelled":0})J") != std::string::npos);
  CHECK(blocked.out.find(R"J("claims":{"live":1,"stale":0})J") != std::string::npos);
  CHECK(blocked.out.find(R"J("blocked_by":["1 live claim(s) still active on plan tasks"])J") != std::string::npos);

  // Snapshot rather than hardcode: `task done 3` ran plan-304's
  // auto-promotion and moved plan 6 from `draft` to `active`, so a literal
  // "draft" here would fail for a reason that has nothing to do with
  // closeout. What this asserts is that the REFUSED apply changed nothing.
  auto const before  = text(fx, "select status from plans where id=6");
  auto const applied = dispatch(fx, {"plan", "closeout", "6"});
  CHECK(applied.code == 3);
  CHECK(applied.err == "error: plan 6 is not ready to close (1 reason(s))\n");
  CHECK(text(fx, "select status from plans where id=6") == before);
  CHECK(before != "done");
}

TEST_CASE("finalization slugs are labelled in both renderings and never block alone", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("finalization");
  seed(fx);

  auto const json = dispatch(fx, {"plan", "closeout", "7", "--dry-run", "--json"});
  CHECK(json.out.find(R"J("tasks":{"open":4,"done":0,"cancelled":0})J") != std::string::npos);
  CHECK(json.out.find(R"J("finalization_tasks":3)J") != std::string::npos);
  // Four open tasks block, not three: the label does not exempt anything.
  CHECK(json.out.find(R"J("blocked_by":["4 open task(s) on plan (todo/doing/blocked)"])J") != std::string::npos);

  auto const text_mode = dispatch(fx, {"plan", "closeout", "7", "--dry-run"});
  CHECK(text_mode.out.find("  tasks:       open=4  done=0  cancelled=0\n"
                           "    (finalization tasks: 3 — merge/reconcile/finalize-prefixed)\n") != std::string::npos);

  // Paired absence: plan 1 has no such tasks, and the indented line is GONE
  // rather than printed with a zero.
  auto const none = dispatch(fx, {"plan", "closeout", "1", "--dry-run"});
  CHECK(none.out.find("finalization tasks") == std::string::npos);
}

TEST_CASE("apply closes the plan, writes one audit row, and is idempotent", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("apply");
  seed(fx);

  auto const applied = dispatch(fx, {"plan", "closeout", "1"});
  CHECK(applied.code == 0);
  CHECK(applied.err.empty());
  CHECK(applied.out == std::format("plan 1: marked done\n"
                                   "\n"
                                   "hard gate:\n"
                                   "  tasks:       open=0  done=0  cancelled=0\n"
                                   "  descendants: open=0  terminal=0\n"
                                   "  claims:      live=0  stale=0\n"
                                   "{}",
                                   k_none_evidence_text));

  CHECK(text(fx, "select status from plans where id=1") == "done");
  REQUIRE(scalar(fx, "select count(*) from audit_log where summary like 'closeout%'") == 1);
  CHECK(text(fx, "select summary from audit_log where summary like 'closeout%'") ==
        "closeout plan 1: → done; tasks done=0 cancelled=0; descendants terminal=0");
  CHECK(text(fx, "select verb from audit_log where summary like 'closeout%'") == "status_change");

  // Re-running is a no-op: no second audit row, and the report drops into
  // the already-terminal shape with an EMPTY `git_evidence` where the run
  // above emitted the one-entry synthetic. Same plan, same database.
  auto const again = dispatch(fx, {"plan", "closeout", "1", "--json"});
  CHECK(again.code == 0);
  CHECK(again.out == R"J({"plan":1,"ready":true,"applied":false,"hard_evidence":{)J"
                     R"J("tasks":{"open":0,"done":0,"cancelled":0},"descendants":{"open":0,"terminal":0},)J"
                     R"J("claims":{"live":0,"stale":0},"finalization_tasks":0},"blocked_by":[],)J"
                     R"J("git_evidence":[],"epic_merge":null,"warnings":[]}
)J");
  CHECK(scalar(fx, "select count(*) from audit_log where summary like 'closeout%'") == 1);

  // The already-terminal TEXT arm has no git-evidence section at all.
  auto const again_text = dispatch(fx, {"plan", "closeout", "1"});
  CHECK(again_text.out == "plan 1: ready (already terminal — no change)\n"
                          "\n"
                          "hard gate:\n"
                          "  tasks:       open=0  done=0  cancelled=0\n"
                          "  descendants: open=0  terminal=0\n"
                          "  claims:      live=0  stale=0\n");
  CHECK(again_text.out.find("git evidence") == std::string::npos);
}

TEST_CASE("check-merge is inert without locality data, in both renderings", "[cmd][plan][closeout]") {
  auto const fx = make_fixture("checkmerge");
  seed(fx);

  // No claim in this fixture carries a `repo_root`, so the roll-up is
  // `null` rather than a zero-valued object — and the flag changes NOTHING
  // else, which the byte comparison against the no-flag run establishes.
  REQUIRE(scalar(fx, "select count(*) from agent_work_claims where repo_root is not null") == 0);

  auto const with_flag    = dispatch(fx, {"plan", "closeout", "1", "--dry-run", "--check-merge", "--json"});
  auto const without_flag = dispatch(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(with_flag.code == 0);
  CHECK(with_flag.out.find(R"J("epic_merge":null)J") != std::string::npos);
  CHECK(with_flag.out == without_flag.out);

  // ...and the text arm prints no epic section either.
  auto const text_mode = dispatch(fx, {"plan", "closeout", "1", "--dry-run", "--check-merge"});
  CHECK(text_mode.out.find("epic-branch merge check") == std::string::npos);
}

TEST_CASE("plan closeout is dispatched rather than refused at 64", "[cmd][plan][closeout]") {
  // Discrimination against the whole unported class: before this task the
  // same argv answered `error: plan closeout: not implemented in this
  // build` at exit 64. The count gate in dispatch.t.cpp pins the inventory;
  // this pins the leaf's own behaviour from the other side.
  auto const fx = make_fixture("dispatched");
  seed(fx);

  auto const result = dispatch(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(result.code != 64);
  CHECK(result.err.find("not implemented") == std::string::npos);
  CHECK(result.out.starts_with(R"J({"plan":1,)J"));
}

// ===========================================================================
// The diagnose section (plan 1132, task 7384)
// ===========================================================================

namespace {

/// @brief An anchor (plan 1) whose only milestone (plan 2) has one finished task, so the anchor is
/// ready to close and nothing is left for the diagnosis to find.
void seed_closable_anchor(const fixture& fx) {
  REQUIRE(dispatch_raw(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch_raw(fx, {"plan", "create", "Anchor", "--scope", "global", "--slug", "anchor", "--json"}).code == 0);
  REQUIRE(dispatch_raw(fx, {"plan", "create", "Milestone", "--scope", "global", "--slug", "milestone", "--parent", "1", "--json"})
              .code == 0);
  REQUIRE(dispatch_raw(fx, {"task", "add", "Only task", "--plan", "2", "--scope", "global", "--no-editor", "--json"}).code == 0);
  REQUIRE(dispatch_raw(fx, {"task", "update", "1", "--status", "doing"}).code == 0);
  REQUIRE(dispatch_raw(fx, {"task", "done", "1"}).code == 0);
  REQUIRE(text(fx, "select status from plans where id = 2") == "done");
}

/// @brief The 7337 shape: a claim left `active` and lapsed on a task that finished.
void seed_lapsed_claim(const fixture& fx) {
  raw_sql(fx, "insert into sessions (vendor) values ('probe')");
  raw_sql(fx, "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, "
              "lease_expires_at) values ('lapsed-1', 1, 'task', 1, 'active', 'probe', '2000-01-01T00:00:00.000Z')");
}

} // namespace

TEST_CASE("a clean anchor closeout prints diagnose: clean after one newline on dry-run and apply",
          "[cmd][plan][closeout][diagnose]") {
  auto const fx = make_fixture("diag_clean");
  seed_closable_anchor(fx);

  auto const dry = dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run"});
  CHECK(dry.code == 0);
  CHECK(dry.err.empty());
  CHECK(dry.out.ends_with("\ndiagnose: clean\n"));
  CHECK(dry.out.starts_with("[dry-run] plan 1: ready to close (no change made)\n"));
  // One newline between the report and the section: the report's own final newline, then one more.
  CHECK(dry.out.find("\n\ndiagnose: clean\n") != std::string::npos);
  CHECK(text(fx, "select status from plans where id = 1") != "done");

  auto const dry_json = dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(dry_json.code == 0);
  auto const key = dry_json.out.find(R"(,"diagnose":{"plan_id":1,"state":"clean","outcome":"ok")");
  REQUIRE(key != std::string::npos);
  CHECK(dry_json.out.ends_with("}}\n"));
  CHECK(dry_json.out.find("\"diagnose\"", key + 12) == std::string::npos);
  CHECK(dry_json.out.find('\n') == dry_json.out.size() - 1);

  auto const applied = dispatch_raw(fx, {"plan", "closeout", "1"});
  CHECK(applied.code == 0);
  CHECK(applied.out.starts_with("plan 1: marked done\n"));
  CHECK(applied.out.ends_with("\ndiagnose: clean\n"));
  CHECK(text(fx, "select status from plans where id = 1") == "done");
}

TEST_CASE("closeout on an already done plan still diagnoses, in both renderings", "[cmd][plan][closeout][diagnose]") {
  auto const fx = make_fixture("diag_done");
  seed_closable_anchor(fx);
  REQUIRE(dispatch_raw(fx, {"plan", "closeout", "1"}).code == 0);

  auto const again = dispatch_raw(fx, {"plan", "closeout", "1"});
  CHECK(again.code == 0);
  CHECK(again.out.starts_with("plan 1: ready (already terminal — no change)\n"));
  CHECK(again.out.ends_with("\ndiagnose: clean\n"));

  auto const again_json = dispatch_raw(fx, {"plan", "closeout", "1", "--json"});
  CHECK(again_json.code == 0);
  CHECK(again_json.out.find(R"("applied":false)") != std::string::npos);
  CHECK(again_json.out.find(R"(,"diagnose":{"plan_id":1,"state":"clean")") != std::string::npos);
  CHECK(again_json.out.ends_with("}}\n"));
}

TEST_CASE("an anomaly is shown without changing ready, blocked_by or the exit status", "[cmd][plan][closeout][diagnose]") {
  auto const fx = make_fixture("diag_finding");
  seed_closable_anchor(fx);
  auto const control = dispatch(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  seed_lapsed_claim(fx);

  auto const dry = dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run"});
  CHECK(dry.code == 0);
  CHECK(dry.out.find("\ndiagnose: 1 finding(s)\n") != std::string::npos);
  CHECK(dry.out.find("error claim-superseded-active claim:1 -> planar-agent abort --claim <token>\n") != std::string::npos);

  auto const json = dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.find(R"("ready":true)") != std::string::npos);
  CHECK(json.out.find(R"("blocked_by":[])") != std::string::npos);
  CHECK(json.out.find(R"(,"diagnose":{"plan_id":1,"state":"findings","outcome":"ok")") != std::string::npos);
  CHECK(json.out.find(R"("check":"claim-superseded-active")") != std::string::npos);
  (void)control;

  auto const applied = dispatch_raw(fx, {"plan", "closeout", "1"});
  CHECK(applied.code == 0);
  CHECK(text(fx, "select status from plans where id = 1") == "done");
}

TEST_CASE("findings neither mask nor change a hard-gate refusal", "[cmd][plan][closeout][diagnose]") {
  auto const fx = make_fixture("diag_refusal");
  seed_closable_anchor(fx);
  REQUIRE(dispatch_raw(fx, {"task", "add", "Open task", "--plan", "2", "--scope", "global", "--no-editor", "--json"}).code == 0);
  seed_lapsed_claim(fx);

  auto const refused = dispatch_raw(fx, {"plan", "closeout", "1", "--json"});
  CHECK(refused.code == 3);
  CHECK(refused.err == "error: plan 1 is not ready to close (2 reason(s))\n");
  CHECK(refused.out.find(R"("ready":false)") != std::string::npos);
  CHECK(
      refused.out.find(
          R"J("blocked_by":["1 open task(s) on plan (todo/doing/blocked)","1 open descendant plan(s) (draft/active/paused)"])J") !=
      std::string::npos);
  CHECK(refused.out.find(R"("check":"claim-superseded-active")") != std::string::npos);
  CHECK(text(fx, "select status from plans where id = 1") != "done");
}

TEST_CASE("a diagnosis that cannot run is reported and closeout proceeds", "[cmd][plan][closeout][diagnose]") {
  auto const fx = make_fixture("diag_unavailable");
  seed_closable_anchor(fx);
  raw_sql(fx, "drop table sync_events");

  auto const dry = dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run"});
  CHECK(dry.code == 0);
  CHECK(dry.out.ends_with("\ndiagnose: unavailable (query-failed)\n"));
  CHECK(dry.out.starts_with("[dry-run] plan 1: ready to close (no change made)\n"));

  auto const json = dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.find(R"(,"diagnose":{"plan_id":1,"state":"unavailable","outcome":"unavailable","reason":"query-failed")") !=
        std::string::npos);

  auto const applied = dispatch_raw(fx, {"plan", "closeout", "1"});
  CHECK(applied.code == 0);
  CHECK(applied.out.ends_with("\ndiagnose: unavailable (query-failed)\n"));
  CHECK(text(fx, "select status from plans where id = 1") == "done");
}

TEST_CASE("a dry-run diagnosis writes nothing", "[cmd][plan][closeout][diagnose]") {
  auto const fx = make_fixture("diag_nowrite");
  seed_closable_anchor(fx);
  seed_lapsed_claim(fx);

  auto const read_all = [&] {
    std::ifstream file(fx.db_path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  };
  auto const before = read_all();
  REQUIRE_FALSE(before.empty());
  REQUIRE(dispatch_raw(fx, {"plan", "closeout", "1", "--dry-run"}).code == 0);
  CHECK(read_all() == before);
}
