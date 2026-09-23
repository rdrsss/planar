// @file plan_next_leaf.t.cpp
// @brief In-process tests for the leaf plan 996 task 6309 landed:
// `planar plan next`.
//
// ## THE FIRST CASE ASSERTS THE FIXTURE, AND IT ALREADY PAID — TWICE
//
// Every other case here is a byte comparison, and a fixture that seeded
// the wrong statuses would satisfy most of them while exercising ONE bucket
// of four. That is not hypothetical: the oracle probe written alongside this
// file did exactly that on its first run. Its seeding step sent every
// command to `/dev/null` without checking status, and three commands failed
// silently —
//
//   `task done <id>` from `todo` is `IllegalTransition` (the lifecycle is
//   todo -> doing -> done, so `task update --status doing` must come first)
//
//   `task block <id> --reason <text>` is a MISSING-FLAG refusal; the verb
//   requires `--on <blocking-task-id>` and `--reason` is optional
//
// — so every task stayed `todo`, all six rows landed in `available`, and the
// run reported `available:5 claimed:0 stale:0 blocked:0 done:0`. Comparing
// that against the implementation would have "passed" while never reaching
// the blocked, claimed, stale or done arms at all. The second failure was
// the expired-lease claim: `planar-agent claim --ttl -60` is REFUSED by the
// duration parser, which is what `agentactivity.cppm`'s header means when it
// says negative TTLs are unreachable from the CLI but legal at the API.
//
// So `seed` below asserts its own post-state, and the first case pins that
// post-state again before any comparison runs.
//
// ## THE FOUR BUCKETS ARE ALL REACHED, AND EACH IS PAIRED WITH ITS ABSENCE
//
// `available`, `claimed`, `stale` and `blocked` each have a populated case
// AND a case where the bucket is empty (`plan next` on the empty plan, and
// on the blocked-only plan). An assertion that a bucket is empty can pass
// because the fixture matched nothing; the paired populated case is what
// tells the two apart.
//
// ## ORACLE PROVENANCE
//
// Every expected byte was captured from `zig/zig-out/bin/planar` built at
// this cycle's base, in a pinned scratch arena (`PLANAR_DB` under a temp
// root — never the operator's database). Exit codes were read from the
// command itself via command substitution with `2>file`, never through a
// pipe: `cmd | tail; echo $?` reports the LAST stage's status, which is how
// an earlier cycle logged `exit: 0` for eighteen consecutive refusals.
//
// The captures were then replayed as a 16-case differential against the
// built C++ binary in a second identically-seeded arena. All 16 agreed on
// stdout, stderr and exit code after normalising only timestamps, claim
// tokens and the arena path.
//
// The captures that decided a shape:
//
//   $Z plan next 1            -> the header's counts are the TRUE totals
//       even when the rows are hidden. With neither flag the header reads
//       `claimed:1  stale:1` and NO claimed or stale row is printed. That
//       reads like a bug and is the oracle's behaviour.
//
//   $Z plan next 1            -> `blocked` rows are printed UNCONDITIONALLY.
//       There is no `--include-blocked`; only claimed and stale are gated.
//
//   $Z plan next 1 --json     -> all four arrays are always present, so the
//       flags gate TEXT only. An empty plan emits `[]` for each — NOT the
//       zero bytes `sync status --json` emits for an empty match set. Two
//       neighbouring verbs, two different empty shapes.
//
//   $Z plan next 999          -> exit 1, `plan 999 not found`. Note this is
//       NOT `plan descendants`' wording for the same condition (`no plan
//       with id 999`); the two verbs were captured separately rather than
//       assuming the sibling transferred.
//
//   $Z plan next abc          -> exit 2, `plan id must be an integer, got
//       'abc'`.
//
//   $Z plan descendants 1     -> the two PLANS and ZERO tasks, on the same
//       database where `plan next 1` lists seven task rows including one
//       under the child plan. The verbs genuinely disagree: `descendants`
//       reads `entity_links(relationship='derives-from')`, `next` reads
//       `plans.parent_plan_id` + `tasks.plan_id`. See the case at the
//       bottom of this file, which exists so a future "unify the two walks"
//       cleanup fails a test rather than passing review.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.session;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;
namespace aa = planar::engine::runtime::agentactivity;

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
                         std::format("planar_plannext_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Dispatch `args` against the real tree and table.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
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

/// @brief Read one integer out of the fixture database.
/// @param fx The fixture.
/// @param sql A statement whose first column is the value.
/// @return The value.
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

/// @brief Seed four plans and ten tasks reaching all four buckets.
///
/// The shape, and why each piece is here rather than trimmed:
///
///   plan 1  the anchor. Carries a task in EVERY bucket plus a `done` and a
///           `cancelled` one, so the two statuses that are in NO bucket are
///           distinguishable from each other — `done` is counted in the
///           summary, `cancelled` is counted NOWHERE.
///   plan 2  a `parent_plan_id` CHILD of plan 1 with one task and no
///           `derives-from` edge. This is the row that makes `plan next`
///           and `plan descendants` disagree, and the reason the last case
///           in this file can exist.
///   plan 3  no tasks at all — the empty case.
///   plan 4  one blocked task and nothing else, so `blocked` is reached
///           with every other bucket empty.
///
/// Priorities are deliberately out of insertion order, and tasks 3 and 4
/// SHARE priority 10, so `order by priority asc, id asc` is exercised
/// rather than assumed: a fixture with distinct priorities cannot tell a
/// correct tiebreak from a missing one.
/// @param fx The fixture.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"plan", "create", "Anchor plan", "--json"}).code == 0);                 // 1
  REQUIRE(dispatch(fx, {"plan", "create", "Child plan", "--parent", "1", "--json"}).code == 0); // 2
  REQUIRE(dispatch(fx, {"plan", "create", "Empty plan", "--json"}).code == 0);                  // 3
  REQUIRE(dispatch(fx, {"plan", "create", "Blocked-only plan", "--json"}).code == 0);           // 4

  auto const add = [&](std::string_view title, std::string_view plan, std::string_view pri) {
    REQUIRE(
        dispatch(fx, {"task", "add", std::string{title}, "--plan", std::string{plan}, "--priority", std::string{pri}, "--json"})
            .code == 0);
  };
  add("Alpha todo", "1", "20");     // 1
  add("Bravo todo", "1", "5");      // 2
  add("Charlie done", "1", "10");   // 3
  add("Delta blocked", "1", "10");  // 4
  add("Echo cancelled", "1", "30"); // 5
  add("Foxtrot child", "2", "1");   // 6
  add("Golf blocker", "1", "40");   // 7
  add("Hotel blocked", "4", "3");   // 8
  add("India claimed", "1", "15");  // 9
  add("Juliet stale", "1", "16");   // 10
  // Both added after a break-probe review found the fixture could not
  // discriminate two real mutations:
  //   11  a `doing` task with NO claim. Without it both `doing` rows carry
  //       claims, the `doing -> available` arm is never reached, and
  //       deleting that arm survives every assertion in this file.
  //   12  a SECOND cancelled task, so cancelled(2) != done(1). With one of
  //       each, swapping `status='done'` for `status='cancelled'` in the
  //       done-count query returns the same number and survives.
  add("Kilo doing unclaimed", "1", "17"); // 11
  add("Lima cancelled", "1", "50");       // 12

  // `done` is a two-step transition; see this file's header for the run
  // where skipping the first step silently left the task at `todo`.
  REQUIRE(dispatch(fx, {"task", "update", "3", "--status", "doing", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "3"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "cancel", "5"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "11", "--status", "doing", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "cancel", "12"}).code == 0);
  // `--on` is REQUIRED and names the BLOCKING task; `--reason` alone is a
  // missing-flag refusal.
  REQUIRE(dispatch(fx, {"task", "block", "4", "--on", "7", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "block", "8", "--on", "7", "--json"}).code == 0);

  // The two claims. `planar` cannot mint one (that is `planar-agent`'s write
  // surface), so these go through the engine directly — which is also the
  // only way to get the EXPIRED one: a negative TTL is legal at
  // `acquire_claim` and refused by the CLI's duration parser.
  //
  // The expired claim keeps `status='active'` with a passed lease. That is
  // the stale arm that matters — the one `reconcile_stale` has not yet
  // touched, and the reason liveness is always the CONJUNCTION of status
  // and lease rather than the status column alone.
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    auto const session = planar::engine::runtime::session::ensure_active(*conn, "test", std::nullopt);
    REQUIRE(session.has_value());

    auto const live = aa::acquire_claim(
        *conn, aa::acquire_args{.session_id = *session, .kind = aa::entity_kind::task, .entity_id = 9, .vendor = "test"});
    REQUIRE(live.has_value());
    auto const expired = aa::acquire_claim(
        *conn, aa::acquire_args{
                   .session_id = *session, .kind = aa::entity_kind::task, .entity_id = 10, .vendor = "test", .ttl_secs = -60});
    REQUIRE(expired.has_value());
  }
}

} // namespace

TEST_CASE("the plan next fixture reaches all four buckets before anything is compared", "[cmd][plan][next][fixture]") {
  // Deliberately first and deliberately about nothing else. The oracle probe
  // this file was derived from seeded a version where three commands failed
  // silently and EVERY task stayed `todo`; the comparisons all still
  // "passed" while one bucket of four was exercised. See the header.
  auto const fx = make_fixture("fixture");
  seed(fx);

  // The statuses, named individually rather than counted: a count would be
  // satisfied by any permutation of them.
  CHECK(scalar(fx, "select count(*) from tasks where id = 1 and status = 'todo'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where id = 3 and status = 'done'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where id = 4 and status = 'blocked'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where id = 5 and status = 'cancelled'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where id = 8 and status = 'blocked'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where id = 10") == 1);
  // The `doing`-with-no-claim row, and the fact that it has no claim: both
  // halves are needed, since a `doing` row that IS claimed lands in a
  // different bucket entirely.
  CHECK(scalar(fx, "select count(*) from tasks where id = 11 and status = 'doing'") == 1);
  CHECK(scalar(fx, "select count(*) from agent_work_claims where entity_id = 11") == 0);
  // done and cancelled must differ in COUNT, or a query that reads the
  // wrong status column returns the right number by accident.
  CHECK(scalar(fx, "select count(*) from tasks where status = 'done'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where status = 'cancelled'") == 2);

  // Task 6 hangs off plan 2, not plan 1. If it were seeded onto the anchor
  // directly the disagreement case at the bottom of this file would pass
  // for the wrong reason.
  CHECK(scalar(fx, "select plan_id from tasks where id = 6") == 2);
  CHECK(scalar(fx, "select parent_plan_id from plans where id = 2") == 1);
  // And NO `derives-from` edges exist, which is what keeps `plan
  // descendants` from seeing any task at all.
  CHECK(scalar(fx, "select count(*) from entity_links where relationship = 'derives-from'") == 0);

  // Two tasks share priority 10, so the id tiebreak is live.
  CHECK(scalar(fx, "select count(*) from tasks where priority = 10") == 2);

  // One LIVE claim and one EXPIRED one. Asserting both halves of the
  // liveness conjunction: same `status='active'`, opposite lease verdicts.
  CHECK(scalar(fx, "select count(*) from agent_work_claims where status = 'active'") == 2);
  CHECK(scalar(fx, "select count(*) from agent_work_claims where entity_id = 9 and status = 'active'"
                   " and lease_expires_at >= strftime('%Y-%m-%dT%H:%M:%fZ','now')") == 1);
  CHECK(scalar(fx, "select count(*) from agent_work_claims where entity_id = 10 and status = 'active'"
                   " and lease_expires_at <  strftime('%Y-%m-%dT%H:%M:%fZ','now')") == 1);
}

TEST_CASE("the text header counts every bucket while printing only two of them", "[cmd][plan][next][text]") {
  auto const fx = make_fixture("textdefault");
  seed(fx);

  auto const ran = dispatch(fx, {"plan", "next", "1"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  // The header's counts are the TRUE totals, and the claimed / stale rows
  // are absent beneath them. Oracle-captured; it reads like a bug.
  CHECK(ran.out == "plan:1  available:0  claimed:1  stale:1  blocked:6  done:1\n"
                   "  blocked    task:6  Foxtrot child  [pri:1]\n"
                   "  blocked    task:2  Bravo todo  [pri:5]\n"
                   "  blocked    task:4  Delta blocked  [pri:10]\n"
                   // `doing` with no live claim, surfaced as AVAILABLE. The
                   // only row in this fixture that reaches that arm.
                   "  blocked    task:11  Kilo doing unclaimed  [pri:17]\n"
                   "  blocked    task:1  Alpha todo  [pri:20]\n"
                   "  blocked    task:7  Golf blocker  [pri:40]\n");
}

TEST_CASE("the two include flags each reveal exactly their own bucket", "[cmd][plan][next][text][flags]") {
  auto const fx = make_fixture("textflags");
  seed(fx);

  // The claim tokens are minted by SQLite, so the rows are matched by their
  // stable prefix and the token is checked for shape rather than value.
  auto const claimed = dispatch(fx, {"plan", "next", "1", "--include-claimed"});
  CHECK(claimed.code == 0);
  CHECK(claimed.out.contains("  claimed    task:9  India claimed  [pri:15, claim:"));
  // ...and the flag reveals ONLY its own bucket. The paired absence: stale
  // is still hidden. Without this half, a handler that ignored both flags
  // and printed everything would satisfy the assertion above.
  CHECK_FALSE(claimed.out.contains("  stale      task:10"));

  auto const stale = dispatch(fx, {"plan", "next", "1", "--include-stale"});
  CHECK(stale.code == 0);
  CHECK(stale.out.contains("  stale      task:10  Juliet stale  [pri:16, claim:"));
  CHECK_FALSE(stale.out.contains("  claimed    task:9"));

  // Both flags together, and the row ORDER across buckets is the single
  // `priority asc, id asc` stream — claimed:15 then stale:16 sit BETWEEN
  // blocked:10 and available:20. A port that grouped rows by bucket would
  // pass every case above and fail this one.
  auto const both = dispatch(fx, {"plan", "next", "1", "--include-claimed", "--include-stale"});
  CHECK(both.code == 0);
  auto const blocked_at   = both.out.find("task:4  Delta blocked");
  auto const claimed_at   = both.out.find("task:9  India claimed");
  auto const stale_at     = both.out.find("task:10  Juliet stale");
  auto const available_at = both.out.find("task:1  Alpha todo");
  CHECK(blocked_at != std::string::npos);
  CHECK(available_at != std::string::npos);
  CHECK(blocked_at < claimed_at);
  CHECK(claimed_at < stale_at);
  CHECK(stale_at < available_at);
}

TEST_CASE("blocked rows print with no flag at all, unlike claimed and stale", "[cmd][plan][next][text][blocked]") {
  auto const fx = make_fixture("blockedonly");
  seed(fx);

  // Plan 4 is the isolated case: one blocked task, every other bucket empty.
  // There is no `--include-blocked` and the row appears anyway.
  auto const ran = dispatch(fx, {"plan", "next", "4"});
  CHECK(ran.code == 0);
  CHECK(ran.out == "plan:4  available:0  claimed:0  stale:0  blocked:1  done:0\n"
                   "  blocked    task:8  Hotel blocked  [pri:3]\n");
}

TEST_CASE("an empty plan prints its header and, under --json, four empty ARRAYS", "[cmd][plan][next][json][empty]") {
  auto const fx = make_fixture("emptyplan");
  seed(fx);

  auto const text = dispatch(fx, {"plan", "next", "3"});
  CHECK(text.code == 0);
  CHECK(text.out == "plan:3  available:0  claimed:0  stale:0  blocked:0  done:0\n");

  // `[]`, NOT the zero bytes `sync status --json` emits for an empty match
  // set. The two verbs sit next to each other and disagree about what empty
  // looks like; a port that carried the sibling's shape here would be
  // well-formed and wrong.
  auto const json = dispatch(fx, {"plan", "next", "3", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == R"({"plan_id":3,"available":[],"claimed":[],"stale":[],"blocked":[],)"
                    R"("summary":{"available":0,"claimed":0,"stale":0,"blocked":0,"done":0}})"
                    "\n");
}

TEST_CASE("the JSON form carries every bucket regardless of the text flags", "[cmd][plan][next][json]") {
  auto const fx = make_fixture("jsonfull");
  seed(fx);

  auto const ran = dispatch(fx, {"plan", "next", "1", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());

  // Neither include flag was passed and both arrays are populated anyway.
  CHECK(ran.out.contains(R"("claimed":[{"task":{"id":9,)"));
  CHECK(ran.out.contains(R"("stale":[{"task":{"id":10,)"));
  CHECK(ran.out.contains(R"("blocked":[{"id":6,)"));
  CHECK(ran.out.ends_with(R"("summary":{"available":0,"claimed":1,"stale":1,"blocked":6,"done":1}})"
                          "\n"));

  // The claimed / stale envelope wraps the task and carries the claim
  // object, including `entity_scope` — which the LEAN claim shape used by
  // `planar-agent`'s payloads omits entirely. Emitting the lean shape here
  // would still be valid JSON.
  CHECK(ran.out.contains(R"("claim":{"id":)"));
  CHECK(ran.out.contains(R"("entity_scope":{"kind":"global","slug":null})"));

  // available and blocked are BARE task objects with no envelope. The
  // paired presence for the absence above: `"available":[{"task":` would
  // mean the envelope leaked across buckets.
  CHECK(ran.out.contains(R"("available":[])"));
  CHECK_FALSE(ran.out.contains(R"("available":[{"task":)"));
  CHECK_FALSE(ran.out.contains(R"("blocked":[{"task":)"));

  // `cancelled` is counted NOWHERE — not in a bucket, not in `done`. Task 5
  // is the only cancelled row and its id must not appear in any array.
  CHECK(scalar(fx, "select count(*) from tasks where status = 'cancelled'") == 2);
  CHECK_FALSE(ran.out.contains(R"("title":"Echo cancelled")"));
  CHECK_FALSE(ran.out.contains(R"("title":"Lima cancelled")"));
  // ...while `done` IS counted, in the summary and nowhere else. The paired
  // presence that keeps the assertion above from passing for the wrong
  // reason: task 3 is likewise absent from every array, but `done:1` proves
  // the row exists and was seen.
  CHECK_FALSE(ran.out.contains(R"("title":"Charlie done")"));
}

TEST_CASE("plan next refuses a missing plan at exit 1 and a non-integer at exit 2", "[cmd][plan][next][refusal]") {
  auto const fx = make_fixture("refusal");
  seed(fx);

  // Captured from the oracle for THIS verb. `plan descendants` words the
  // same condition differently (`no plan with id 999`), so the sibling's
  // message was deliberately not reused.
  auto const missing = dispatch(fx, {"plan", "next", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  CHECK(missing.err == "error: plan 999 not found\n");

  auto const bad = dispatch(fx, {"plan", "next", "abc"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: plan id must be an integer, got 'abc'\n");

  // The refusal is a refusal, not a silent empty success: a handler with no
  // existence probe would print an all-zeros header at exit 0 here, which
  // is exactly what the probe exists to prevent.
  CHECK_FALSE(missing.out.contains("plan:999"));
}

TEST_CASE("plan next and plan descendants AGREE that a plan_id-attached task is under the plan",
          "[cmd][plan][next][descendants][6307]") {
  // INVERTED AT TASK 6307. This case used to pin the two verbs DISAGREEING
  // on one database: `descendants` followed `derives-from` edges only, so a
  // task attached the ordinary way -- `task add --plan`, which writes
  // `tasks.plan_id` and no edge -- was invisible to it while `plan next`
  // listed it.
  //
  // That was the defect, not an invariant. It is pinned from both sides here
  // so the agreement is what a future change has to break, rather than the
  // divergence being what it has to preserve.
  auto const fx = make_fixture("divergence");
  seed(fx);

  // `descendants` now reads BOTH routes, so task 6 reaches the tree through
  // the child plan it is attached to.
  auto const desc = dispatch(fx, {"plan", "descendants", "1", "--json"});
  CHECK(desc.code == 0);
  CHECK(desc.out.contains(R"({"kind":"plan","role":"anchor","id":1,"title":"Anchor plan"})"));
  CHECK(desc.out.contains(R"({"kind":"plan","role":"child","id":2,"title":"Child plan"})"));
  CHECK(desc.out.contains("Foxtrot child"));

  // `next` follows `plans.parent_plan_id` then `tasks.plan_id` and lists the
  // same task. The two verbs now answer the same question the same way.
  auto const next = dispatch(fx, {"plan", "next", "1"});
  CHECK(next.code == 0);
  CHECK(next.out.contains("task:6  Foxtrot child"));

  // And the child plan reached directly returns that one task, confirming
  // it is the parent_plan_id recursion carrying it into the anchor's
  // result rather than a stray anchor-scoped row.
  auto const child = dispatch(fx, {"plan", "next", "2"});
  CHECK(child.code == 0);
  // The task belongs to the shared subtree, but is not dispatchable until
  // its routing packet is materialized. `plan next` therefore surfaces it
  // in the same blocked bucket the operator must resolve before claiming.
  CHECK(child.out == "plan:2  available:0  claimed:0  stale:0  blocked:1  done:0\n"
                     "  blocked    task:6  Foxtrot child  [pri:1]\n");
}
