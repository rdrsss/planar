// @file audit_handoff_readiness_leaf.t.cpp
// @brief In-process tests for `planar audit handoff-readiness`, ported by
// plan 996, task 6329.
//
// ## THE LEAF WAS RECORDED AS "MERELY LARGE"; IT IS THE FAMILY'S SMALLEST
//
// handlers/audit.cppm carried `audit handoff-readiness` as "the one that
// is merely LARGE rather than blocked". The oracle handler is 101 lines —
// smaller than `audit trail`'s two forms, smaller than `audit session` —
// and its single engine dependency, `engine.runtime.resumecheck`, was
// already in the tree when the note was written. That is the second
// over-stated cost in this one file (see the `audit commits` correction
// above it), and both were arrived at the same way: from the family's
// reputation rather than from the leaf's own handler.
//
// ## THE GATE TRUNCATES AND THE DISPLAY ROUNDS, SO THE TEXT CONTRADICTS ITSELF
//
// `ok` is `int64(pct) >= threshold`. Both rendered forms round: `{:.2f}`
// in JSON, `{:.0f}` in text. At two of three tasks passing, `pct` is
// 66.666…, which truncates to 66 for the comparison and rounds to 67 for
// display — so `--threshold 67` prints, literally:
//
//     FAIL: threshold not met (67% < 67%)
//
// That is an ORACLE DEFECT, not a transcription slip, and it is pinned
// here as the contract rather than quietly corrected. The 66/67 pair is
// the whole reason the fixture has three tasks and not two: at two tasks
// every rate is an exact integer and truncation is indistinguishable from
// rounding, so a port that rounded the gate would pass a 1-of-2 fixture.
//
// ## THE ABSENCE CASES ALL HAVE A PRESENT TWIN
//
// Every "does not appear" assertion below is paired with the state where
// it does appear. `done`/`cancelled` tasks are excluded from the scan —
// asserted against a fixture where flipping one changes `total`, not
// against a fixture where the count happens to be right. Likewise the
// resumable/non-resumable split is asserted in both directions, because a
// port that reported EVERY task non-resumable would satisfy a
// failures-only fixture perfectly.
//
// ## THE SCAN IS GLOBAL, AND THAT IS TESTED, NOT ASSUMED
//
// The leaf has no `--scope` flag and the oracle's SQL carries no scope
// predicate. The fixture seeds a SECOND association with its own in-flight
// task and asserts it is counted — an oracle-captured behaviour that the
// sibling verbs' cwd-derived scoping makes genuinely surprising.
//
// ## EVERY CONSTANT HERE CAME FROM `zig/zig-out/bin/planar`
//
// Captured in a pinned scratch arena over an identical fixture, both
// streams read through a pipe and the exit code read outside it, then
// diffed byte-for-byte against this binary across fifteen argument
// combinations.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
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
                         std::format("planar_ahr_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::database>(fx.db_path, err), out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Count rows in one table.
/// @param conn The open connection.
/// @param table The table name.
/// @return The row count.
auto count(planar::db::connection& conn, std::string_view table) -> std::int64_t {
  auto stmt = conn.prepare(std::format("select count(*) from {}", table));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Seed three in-flight tasks, exactly TWO of which are resumable.
///
/// Two-of-three is chosen for the truncation edge (66.666…); see this
/// file's header for why two-of-two would not discriminate.
///
/// Resumability is established through the CLI, not by asserting what
/// `resumecheck` happens to require: a task becomes resumable when it has
/// BOTH a `next_action` and a context snapshot. Task 3 is given neither,
/// so it fails on two checks rather than one — which also proves the
/// handler counts TASKS and not failed checks.
/// @param fx The fixture.
void seed(const fixture& fx) {
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  // ABSOLUTE, never `.` — a literal dot stores a path no cwd-derivation
  // can match and the plan/task writes below would land unscoped.
  CHECK(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Readiness plan", "--json"}).code == 0);
  // `--body` supplied so the write never reaches the editor path.
  CHECK(dispatch(fx, {"task", "add", "T one", "--plan", "1", "--body", "one", "--next-action", "do one", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "T two", "--plan", "1", "--body", "two", "--next-action", "do two", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "T three", "--plan", "1", "--body", "three", "--json"}).code == 0);
  CHECK(dispatch(fx, {"capture", "snapshot", "ctx one", "--task", "1", "--next-action", "do one", "--json"}).code == 0);
  CHECK(dispatch(fx, {"capture", "snapshot", "ctx two", "--task", "2", "--next-action", "do two", "--json"}).code == 0);

  // FIXTURE SELF-CHECK, and it has already earned its keep on this task:
  // the first three seeding attempts used `plan add` / `task add --title`
  // and wrote NOTHING, leaving a fixture against which every assertion
  // below would have passed vacuously.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "tasks") == 3);
  CHECK(count(*conn, "context_snapshots") == 2);

  // And the split itself is asserted through the PUBLIC verb rather than
  // assumed from the seeding: two resumable, one not. Without this a
  // change to what `resumecheck` requires would silently turn the
  // truncation cases below into a different arithmetic.
  CHECK(dispatch(fx, {"resume", "validate", "1", "--json"}).code == 0);
  CHECK(dispatch(fx, {"resume", "validate", "2", "--json"}).code == 0);
  CHECK(dispatch(fx, {"resume", "validate", "3", "--json"}).code == 1);
}

} // namespace

TEST_CASE("handoff-readiness reports the pass rate over in-flight tasks", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("rate");
  seed(fx);

  auto const res = dispatch(fx, {"audit", "handoff-readiness", "--json"});
  // Two of three pass, so the DEFAULT threshold of 90 is not met and the
  // verb refuses — the payload is still written in full.
  CHECK(res.code == 1);
  // The additive --json error envelope (decision 1145, task 6844) is
  // SUPPRESSED here (task 6903): the handler already wrote its own JSON
  // payload to stdout, and appending a second document would break a
  // `json.loads` consumer. One document per stream -- the payload alone.
  CHECK(res.out == R"({"total":3,"passing":2,"failing":1,"percentage":66.67,"threshold":90,"ok":false})"
                   "\n");
  CHECK(res.err == "error: handoff readiness below threshold\n");
}

TEST_CASE("the threshold gate TRUNCATES while the display ROUNDS", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("truncate");
  seed(fx);

  // 66.666… truncates to 66. Threshold 66 passes; 67 does not.
  auto const at66 = dispatch(fx, {"audit", "handoff-readiness", "--threshold", "66", "--json"});
  CHECK(at66.code == 0);
  CHECK(at66.out == R"({"total":3,"passing":2,"failing":1,"percentage":66.67,"threshold":66,"ok":true})"
                    "\n");
  CHECK(at66.err.empty());

  auto const at67 = dispatch(fx, {"audit", "handoff-readiness", "--threshold", "67", "--json"});
  CHECK(at67.code == 1);
  // Task 6903: no envelope appended; the handler's own payload is the
  // whole document.
  CHECK(at67.out == R"({"total":3,"passing":2,"failing":1,"percentage":66.67,"threshold":67,"ok":false})"
                    "\n");

  // THE SELF-CONTRADICTORY LINE. `percentage` renders 67 and the threshold
  // is 67, and it still reports "not met" — because the comparison saw 66.
  // Oracle-captured; reproduced deliberately.
  auto const text67 = dispatch(fx, {"audit", "handoff-readiness", "--threshold", "67"});
  CHECK(text67.code == 1);
  CHECK(text67.out == "handoff-readiness: 2/3 tasks pass (67%, threshold 67%)\n"
                      "  FAIL task:3 \"T three\" [todo]\n"
                      "FAIL: threshold not met (67% < 67%)\n");
}

TEST_CASE("a failing --json run with a payload emits exactly ONE JSON document on stdout",
          "[cmd][audit][handoff-readiness][6903]") {
  // Task 6903: dispatched via `dispatch.cpp`'s `run_tracking_stdout_writes`
  // -- the handler already wrote `{"total":...}` to stdout before
  // returning failure, so the additive --json error envelope is
  // SUPPRESSED. A caller doing `json.loads(proc.stdout)` must see exactly
  // one document, never two concatenated ones.
  auto const fx = make_fixture("onedoc");
  seed(fx);

  auto const res = dispatch(fx, {"audit", "handoff-readiness", "--json"});
  CHECK(res.code == 1);
  // Exactly one newline -- one line, one JSON document. A regression that
  // re-appends the envelope would push this to 2.
  CHECK(std::ranges::count(res.out, '\n') == 1);
  CHECK(res.out.starts_with(R"({"total":3,)"));
  CHECK_FALSE(res.out.contains(R"("error":{"verb")"));
}

TEST_CASE("a failing --json HANDLER that writes NOTHING still gets the envelope, unaffected by task 6903",
          "[cmd][audit][handoff-readiness][6903]") {
  // Contrast case: `resume validate <missing task> --json` runs a real
  // handler that fails WITHOUT writing anything to stdout first (see
  // `resume validate: absent task writes NOTHING to stdout` in
  // handlers.t.cpp). The dispatch-site detection added by task 6903 must
  // not suppress the envelope here -- it is the ONLY document on stdout,
  // and dropping it would silently regress every verb that writes nothing
  // before failing.
  auto const fx = make_fixture("nodoc");
  REQUIRE(dispatch(fx, {"init", "--name", "nodoc", "--json"}).code == 0);

  auto const res = dispatch(fx, {"resume", "validate", "999", "--json"});
  CHECK(res.code == 1);
  CHECK(res.out == planar::cmd::testsupport::json_error_envelope_line("resume validate", "not_found"));
}

TEST_CASE("the FAIL list is not gated on the verdict", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("faillist");
  seed(fx);

  // A PASSING run still lists every failing task, then prints `OK:`. A
  // port that emitted the list only on failure would satisfy every case in
  // the test above and break here.
  auto const res = dispatch(fx, {"audit", "handoff-readiness", "--threshold", "66"});
  CHECK(res.code == 0);
  CHECK(res.out == "handoff-readiness: 2/3 tasks pass (67%, threshold 66%)\n"
                   "  FAIL task:3 \"T three\" [todo]\n"
                   "OK: threshold met\n");
  CHECK(res.err.empty());
}

TEST_CASE("an EMPTY database is ok at any threshold, including 101", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("empty");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);

  // `total == 0` short-circuits AHEAD of the comparison, so the
  // `percentage:0.00` reported alongside is not what was tested. 101 is
  // the case that proves the short-circuit rather than a lenient
  // comparison: no percentage can reach it.
  auto const res = dispatch(fx, {"audit", "handoff-readiness", "--threshold", "101", "--json"});
  CHECK(res.code == 0);
  CHECK(res.out == R"({"total":0,"passing":0,"failing":0,"percentage":0.00,"threshold":101,"ok":true})"
                   "\n");

  auto const text = dispatch(fx, {"audit", "handoff-readiness"});
  CHECK(text.code == 0);
  CHECK(text.out == "handoff-readiness: 0/0 tasks pass (0%, threshold 90%)\n"
                    "OK: threshold met\n");
}

TEST_CASE("out-of-range thresholds are ACCEPTED, not validated", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("range");
  seed(fx);

  // The oracle range-checks nothing. 0 accepts anything; 101 is
  // unreachable. Neither is exit 2 — contrast `report --days 0`, which
  // does refuse. Two sibling verbs, two conventions.
  CHECK(dispatch(fx, {"audit", "handoff-readiness", "--threshold", "0", "--json"}).code == 0);
  CHECK(dispatch(fx, {"audit", "handoff-readiness", "--threshold", "101", "--json"}).code == 1);
}

TEST_CASE("only todo/doing/blocked are scanned, and the excluded twin proves it", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("statuses");
  seed(fx);

  // PRESENT case first: task 3 is `todo`, is counted, and fails.
  auto const before = dispatch(fx, {"audit", "handoff-readiness", "--json"});
  CHECK(before.out.find(R"("total":3)") != std::string::npos);
  CHECK(before.out.find(R"("failing":1)") != std::string::npos);

  // Flip the one failing task out of the in-flight set. If the scan
  // ignored status entirely, `total` would stay 3.
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("update tasks set status = 'cancelled' where id = 3").has_value());
  }

  auto const after = dispatch(fx, {"audit", "handoff-readiness", "--json"});
  // 2 of 2 now — and the rate reaching 100 is what flips the verdict, so
  // this also covers the all-pass arm the other cases never reach.
  CHECK(after.code == 0);
  CHECK(after.out == R"({"total":2,"passing":2,"failing":0,"percentage":100.00,"threshold":90,"ok":true})"
                     "\n");
  CHECK(after.out.find("T three") == std::string::npos);

  auto const text = dispatch(fx, {"audit", "handoff-readiness"});
  // No FAIL lines at all when nothing fails.
  CHECK(text.out == "handoff-readiness: 2/2 tasks pass (100%, threshold 90%)\n"
                    "OK: threshold met\n");
}

TEST_CASE("the scan is GLOBAL and crosses associations", "[cmd][audit][handoff-readiness]") {
  auto const fx = make_fixture("global");
  seed(fx);

  // A second association with its own plan and its own in-flight task.
  // Every sibling verb would scope this away; this leaf counts it.
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "other", ec);
  CHECK(dispatch(fx, {"assoc", "create", "project:other", "--kind", "project", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "add", "project:other", (fx.root / "other").string()}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Other plan", "--scope", "project:other", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "Other task", "--plan", "2", "--scope", "project:other", "--body", "o", "--json"}).code ==
        0);

  // Still dispatched from `proj`, whose derived scope does NOT contain the
  // new task — and `total` rises to 4 regardless.
  auto const res = dispatch(fx, {"audit", "handoff-readiness", "--json"});
  CHECK(res.out.find(R"("total":4)") != std::string::npos);
  CHECK(res.out.find(R"("failing":2)") != std::string::npos);
}
