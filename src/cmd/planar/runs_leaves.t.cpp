// @file runs_leaves.t.cpp
// @brief In-process tests for all TEN `bench` and `run` leaves, wired by
// plan 996, task 6149 (nine of them) and task 6362 (`bench harvest`, the
// last).
//
// ## EVERY CASE ASSERTS DATABASE ROWS, NOT ONLY STDOUT
//
// The same discipline `annotate_leaves.t.cpp` established, for the same
// reason: this family's leaves print almost nothing. `bench event`,
// `bench touch` and `bench finish` ALL print the three bytes `ok\n` on
// success, so a handler that wrote the wrong seq, the wrong touch kind, or
// no row at all is indistinguishable from a correct one on stdout. Rows are
// read back through `run_snapshot` / `event_rows` / `touch_rows`, which
// render SQL NULL as the literal `<NULL>` — the distinction is live in this
// schema (`base_sha` and `config_hash` are `not null` and hold the EMPTY
// STRING for an operational run, while `config_json`, `corpus_repo`,
// `ended_at` and `run_events.payload` are genuinely nullable), and a port
// that confused the two would still print identical JSON only if the row
// underneath happened to be right.
//
// ## THE `--task` FILTER IS PROVEN TO EXCLUDE, WITH SURVIVAL FIRST
//
// `bench start --task` is this tree's FIRST repeatable flag. The filter
// case seeds three tasks with one `task_touch_paths` row each and starts
// three runs from the same plan: unfiltered, `--task 1 --task 3`, and
// `--task 2`. The unfiltered run is asserted FIRST and must carry all
// three — that is what makes the filtered runs' absences mean something,
// and a count-only assertion on the filtered run would pass against an
// engine that had simply written nothing.
//
// It also catches the accessor bug specifically: reading `--task` through
// `flag_string` (last value only) yields task 3 alone where the oracle
// yields 1 AND 3, at exit 0 with identical stdout, since `bench start`
// prints only the uid.
//
// ## THREE UNGUARDED TRANSITIONS ARE PINNED AS ACCEPTED
//
// Probed against the reference binary, not assumed: a SECOND `bench
// finish` overwrites `status` and re-stamps `ended_at`; `bench event` and
// `bench touch` after a finish both append normally. All three exit 0.
// They are pinned here as SUCCESSES so that a later change adding a state
// machine fails loudly rather than silently refusing operations the oracle
// accepts (D2).
//
// ## ORACLE PROVENANCE
//
// Every expected byte below came from running `zig/zig-out/bin/planar`
// against a scratch `PLANAR_DB` in a registered scratch project, captured
// through a PIPE. Capturing into a redirected FILE is unreliable for this
// family: zig's `std.log.err` writer and its buffered `exit.die` writer
// hold independent offsets on a regular file and the two lines come out
// interleaved and truncated. The captures that decided a shape:
//
//   $Z bench start b1 --plan 1 --arm strict --base-sha deadbeef \
//                     --config-hash ch1        exit 0  stdout b'b1\n'  (3 B)
//   $Z bench start b2 ... --arm weird
//        exit 0, stderr b"warn: bench start: unrecognized arm 'weird';
//                         recognized arms: strict, eligibility, grouped\n"
//        ^ a WARNING, and the run is still written. The arm set is
//          recognized, not closed.
//   $Z bench event b1 --kind result --seq 1     exit 0  stdout b'ok\n'
//   $Z bench event b1 --kind dup --seq 1
//        exit 6  stderr b"error: bench event: seq 1 already used for run 'b1'\n"
//   $Z bench touch b1 ... (repeat tuple)
//        exit 1  stderr b'error: bench touch: QueryFailed\n'
//        ^ exit ONE, not six: the same class of UNIQUE violation the two
//          lines above report at exit 6. Not normalized.
//   $Z bench start b1 ... (duplicate uid)
//        exit 6  stderr b"error: bench start: run_uid 'b1' already exists\n"
//   $Z bench show nosuch  exit 1  stderr b"error: bench show: run 'nosuch' not found\n"
//   $Z bench finish b1 --status bogus
//        exit 2  stderr b"error: bench finish: invalid --status 'bogus';
//                         expected completed, aborted, or error\n"
//   $Z run start --plan 1          exit 0  stdout the SAME JSON as with --json
//   $Z run event <uid> --kind step exit 0  stdout b'{"run_uid":...,"seq":1,...}\n'
//        ^ seq AUTO-INCREMENTED here; `bench event` takes the caller's.
//
// The `error: runs.<op> exec failed: StepFailed` line the oracle prints
// alongside every `QueryFailed` is a `std.log.err` artifact and is NOT
// reproduced — the call task 6135 already made for `task.create` (see
// `handlers.t.cpp`'s task-6135 header).

// Include-before-import is deliberate (see db/db.t.cpp): the `bench
// harvest` git fixtures below need `::popen`/`::pclose` from `<cstdio>` in
// the global module fragment.
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A scratch root plus the environment and database path every case
/// in this file dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
///
/// Nothing here reads the real environment, so there is no path by which
/// the operator's `~/.planar/planar.db` can be reached.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_runs_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table inside `fx`.
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
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

/// @brief Open the fixture's database directly, for row assertions.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief Run one statement for its effect.
/// @param conn An open connection.
/// @param sql The statement.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Collect one query's rows, pipe-joining columns and newline-joining
/// rows, with SQL NULL rendered as the literal `<NULL>`.
///
/// The `<NULL>` rendering is the point of this helper. `runs.base_sha` and
/// `runs.config_hash` are `not null` and hold `''` for an operational run,
/// while `config_json`, `corpus_repo`, `ended_at` and `run_events.payload`
/// are nullable. A port that wrote `''` where the oracle writes NULL emits
/// `""` instead of `null` in the JSON — but only a row read distinguishes
/// the two on the leaves that never print the column at all.
/// @param conn An open connection.
/// @param sql The query.
/// @param columns How many columns to read.
/// @return The rendered rows.
auto query(planar::db::connection& conn, std::string_view sql, int columns) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += '\n';
    }
    for (int col = 0; col < columns; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every column of the `runs` rows that is not wall-clock, with the
/// `ended_at` presence folded in as a stable token.
/// @param conn An open connection.
/// @return The rendered rows, one per line, in id order.
auto run_snapshot(planar::db::connection& conn) -> std::string {
  return query(conn,
               R"(select run_uid, cast(plan_id as text), arm, base_sha, config_hash,
                    coalesce(config_json, '<NULL>'), coalesce(corpus_repo, '<NULL>'), status,
                    case when ended_at is null then 'no-end' else 'ended' end
                  from runs order by id)",
               9);
}

/// @brief Every `run_events` row, payload NULL distinguishable from `''`.
/// @param conn An open connection.
/// @return The rendered rows, one per line, in id order.
auto event_rows(planar::db::connection& conn) -> std::string {
  return query(conn, R"(select r.run_uid, cast(e.seq as text), e.kind, coalesce(e.payload, '<NULL>')
                        from run_events e join runs r on r.id = e.run_id order by e.id)",
               4);
}

/// @brief Every `run_touches` row.
/// @param conn An open connection.
/// @return The rendered rows, one per line, in id order.
auto touch_rows(planar::db::connection& conn) -> std::string {
  return query(conn, R"(select r.run_uid, cast(t.task_id as text), t.path, t.kind
                        from run_touches t join runs r on r.id = t.run_id order by t.id)",
               4);
}

/// @brief Seed a registered project, association, plan and three tasks — the
/// minimum a `bench`/`run` case needs, because `runs.plan_id` is an FK.
///
/// Goes through the CLI rather than raw SQL so the fixture exercises the
/// same code paths an operator would. `assoc add` is given the ABSOLUTE
/// project path: a relative `.` reports `added` at exit 0 on BOTH binaries
/// and associates nothing, after which `plan create` refuses at exit 5.
/// That silent no-op cost this file's first draft a vacuous green — every
/// happy-path case ran against a plan that did not exist and "matched" the
/// oracle only because both had failed.
/// @param fx The fixture.
auto seed(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "feat", "--name", "feat", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "feat", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "P1", "--slug", "p1", "--json"}).code == 0);
  for (auto const* title : {"T1", "T2", "T3"}) {
    REQUIRE(dispatch(fx, {"task", "add", title, "--plan", "1", "--json"}).code == 0);
  }
}

} // namespace

TEST_CASE("bench start writes the run row and prints the uid alone", "[cmd][bench]") {
  auto const fx = make_fixture("start");
  seed(fx);

  auto const minimal =
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "deadbeef", "--config-hash", "ch1"});
  CHECK(minimal.code == 0);
  // Three bytes, no JSON, and there is no `--json` flag on this leaf.
  CHECK(minimal.out == "b1\n");
  CHECK(minimal.err.empty());

  auto const full = dispatch(fx, {"bench", "start", "b2", "--plan", "1", "--arm", "eligibility", "--base-sha", "d",
                                  "--config-hash", "c", "--config-json", R"({"k":1})", "--corpus-repo", "/tmp/x"});
  CHECK(full.code == 0);
  CHECK(full.out == "b2\n");

  auto conn = open_db(fx);
  // NULL vs empty is live on this row: `base_sha`/`config_hash` carry the
  // supplied text, `config_json`/`corpus_repo` are NULL on the minimal run
  // and populated on the full one.
  CHECK(run_snapshot(conn) == "b1|1|strict|deadbeef|ch1|<NULL>|<NULL>|running|no-end\n"
                              "b2|1|eligibility|d|c|{\"k\":1}|/tmp/x|running|no-end");
}

TEST_CASE("bench start WARNS on an unrecognized arm and still writes the run", "[cmd][bench]") {
  auto const fx = make_fixture("arm");
  seed(fx);

  auto const warned =
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "weird", "--base-sha", "s", "--config-hash", "c"});
  // The arm set is RECOGNIZED, not closed: exit 0, the row is written, and
  // the message goes to stderr as a `warn:` line.
  CHECK(warned.code == 0);
  CHECK(warned.out == "b1\n");
  CHECK(warned.err == "warn: bench start: unrecognized arm 'weird'; recognized arms: strict, eligibility, grouped\n");

  auto conn = open_db(fx);
  CHECK(run_snapshot(conn) == "b1|1|weird|s|c|<NULL>|<NULL>|running|no-end");
}

TEST_CASE("bench start emits the arm warning BEFORE either refusal", "[cmd][bench]") {
  auto const fx = make_fixture("order");
  seed(fx);

  // Three probes that between them fix the whole order. Each satisfies
  // every earlier check and violates exactly one later, so no single probe
  // could distinguish them.
  auto const both = dispatch(fx, {"bench", "start", "q1", "--plan", "1", "--arm", "s", "--base-sha", "x", "--config-hash", "y",
                                  "--config-json", "notjson", "--task", "notanint"});
  CHECK(both.code == 2);
  CHECK(both.err == "warn: bench start: unrecognized arm 's'; recognized arms: strict, eligibility, grouped\n"
                    "error: bench start: --config-json is not valid JSON: notjson\n");

  auto const json_first = dispatch(fx, {"bench", "start", "q2", "--plan", "1", "--arm", "strict", "--base-sha", "x",
                                        "--config-hash", "y", "--config-json", "notjson", "--task", "notanint"});
  // `--config-json` beats `--task`, with no warning line in front.
  CHECK(json_first.code == 2);
  CHECK(json_first.err == "error: bench start: --config-json is not valid JSON: notjson\n");

  auto const task_only = dispatch(fx, {"bench", "start", "q3", "--plan", "1", "--arm", "strict", "--base-sha", "x",
                                       "--config-hash", "y", "--task", "notanint"});
  CHECK(task_only.code == 2);
  CHECK(task_only.err == "error: bench start: --task value must be an integer, got 'notanint'\n");

  auto conn = open_db(fx);
  // NONE of the three wrote a run. A refusal that had already inserted
  // would still print the same stderr.
  CHECK(run_snapshot(conn).empty());
}

TEST_CASE("bench start --task EXCLUDES, and the excluded rows survive elsewhere", "[cmd][bench]") {
  auto const fx = make_fixture("filter");
  seed(fx);
  {
    auto conn = open_db(fx);
    exec(conn, "insert into task_touch_paths (task_id, repo_id, path) values (1,1,'a.c'),(2,1,'b.c'),(3,1,'c.c')");
  }

  auto const all =
      dispatch(fx, {"bench", "start", "all", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"});
  CHECK(all.code == 0);
  // TWO ids, not one: reading `--task` through a last-value-wins accessor
  // would snapshot task 3 alone, at exit 0 with byte-identical stdout.
  auto const some = dispatch(fx, {"bench", "start", "filt", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash",
                                  "c", "--task", "1", "--task", "3"});
  CHECK(some.code == 0);
  auto const one = dispatch(
      fx, {"bench", "start", "one", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c", "--task", "2"});
  CHECK(one.code == 0);
  auto const none = dispatch(
      fx, {"bench", "start", "none", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c", "--task", "999"});
  CHECK(none.code == 0);

  auto conn = open_db(fx);
  // SURVIVAL FIRST. The unfiltered run carries all three, so the rows the
  // filtered runs omit demonstrably exist and demonstrably match the plan.
  // Asserting the filtered counts alone would pass against an engine that
  // snapshotted nothing at all.
  CHECK(touch_rows(conn) == "all|1|a.c|declared\n"
                            "all|2|b.c|declared\n"
                            "all|3|c.c|declared\n"
                            "filt|1|a.c|declared\n"
                            "filt|3|c.c|declared\n"
                            "one|2|b.c|declared");
  // `none` matched no task and wrote NO touches — but it did write its run.
  CHECK(query(conn, "select run_uid from runs order by id", 1) == "all\nfilt\none\nnone");
}

TEST_CASE("bench event writes the CALLER's seq and refuses a duplicate at exit 6", "[cmd][bench]") {
  auto const fx = make_fixture("event");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  auto const first = dispatch(fx, {"bench", "event", "b1", "--kind", "result", "--seq", "1", "--payload", R"({"a":1})"});
  CHECK(first.code == 0);
  CHECK(first.out == "ok\n");
  auto const bare = dispatch(fx, {"bench", "event", "b1", "--kind", "plain", "--seq", "7"});
  CHECK(bare.code == 0);
  CHECK(bare.out == "ok\n");

  auto const dup = dispatch(fx, {"bench", "event", "b1", "--kind", "dup", "--seq", "1"});
  CHECK(dup.code == 6);
  CHECK(dup.err == "error: bench event: seq 1 already used for run 'b1'\n");

  auto const bad_json = dispatch(fx, {"bench", "event", "b1", "--kind", "k", "--seq", "9", "--payload", "notjson"});
  CHECK(bad_json.code == 2);
  CHECK(bad_json.err == "error: bench event: --payload is not valid JSON: notjson\n");

  auto const missing = dispatch(fx, {"bench", "event", "nosuch", "--kind", "k", "--seq", "1"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: bench event: run 'nosuch' not found\n");

  auto conn = open_db(fx);
  // Exactly the two accepted rows, at the seqs the CALLER chose (7, not 2)
  // — the difference from `run event`. Payload NULL on the bare one.
  CHECK(event_rows(conn) == "b1|1|result|{\"a\":1}\n"
                            "b1|7|plain|<NULL>");
}

TEST_CASE("bench touch reports a repeat tuple at exit 1, NOT the duplicates' exit 6", "[cmd][bench]") {
  auto const fx = make_fixture("touch");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  CHECK(dispatch(fx, {"bench", "touch", "b1", "--task", "1", "--path", "src/a.c", "--kind", "declared"}).code == 0);
  CHECK(dispatch(fx, {"bench", "touch", "b1", "--task", "1", "--path", "src/a.c", "--kind", "actual"}).code == 0);

  // The SAME (run, task, path, kind) tuple. The engine uses `touch`, not
  // `touch_idempotent`, precisely so this reports rather than no-ops.
  auto const repeat = dispatch(fx, {"bench", "touch", "b1", "--task", "1", "--path", "src/a.c", "--kind", "declared"});
  CHECK(repeat.code == 1);
  CHECK(repeat.err == "error: bench touch: QueryFailed\n");

  auto const bad_kind = dispatch(fx, {"bench", "touch", "b1", "--task", "1", "--path", "p", "--kind", "bogus"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: bench touch: invalid --kind 'bogus'; expected declared or actual\n");
  // The refusal is decided before SQLite is opened at all.
  CHECK_FALSE(bad_kind.db_open);

  auto conn = open_db(fx);
  // The differing `kind` makes the second row a DIFFERENT tuple, so both
  // survive; the repeat added nothing.
  CHECK(touch_rows(conn) == "b1|1|src/a.c|declared\n"
                            "b1|1|src/a.c|actual");
}

TEST_CASE("bench finish is UNGUARDED: a second finish overwrites the status", "[cmd][bench]") {
  auto const fx = make_fixture("finish");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  CHECK(dispatch(fx, {"bench", "finish", "b1", "--status", "completed"}).code == 0);
  {
    auto conn = open_db(fx);
    CHECK(run_snapshot(conn) == "b1|1|strict|s|c|<NULL>|<NULL>|completed|ended");
  }

  // Probed against the oracle: exit 0, and the terminal status is replaced.
  auto const second = dispatch(fx, {"bench", "finish", "b1", "--status", "aborted"});
  CHECK(second.code == 0);
  CHECK(second.out == "ok\n");
  // ...and events and touches still append after a finish.
  CHECK(dispatch(fx, {"bench", "event", "b1", "--kind", "post", "--seq", "2"}).code == 0);
  CHECK(dispatch(fx, {"bench", "touch", "b1", "--task", "1", "--path", "late.c", "--kind", "actual"}).code == 0);

  auto const bad = dispatch(fx, {"bench", "finish", "b1", "--status", "bogus"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: bench finish: invalid --status 'bogus'; expected completed, aborted, or error\n");
  CHECK_FALSE(bad.db_open);

  auto conn = open_db(fx);
  CHECK(run_snapshot(conn) == "b1|1|strict|s|c|<NULL>|<NULL>|aborted|ended");
  CHECK(event_rows(conn) == "b1|2|post|<NULL>");
  CHECK(touch_rows(conn) == "b1|1|late.c|actual");
}

TEST_CASE("bench start refuses a duplicate uid at exit 6 and writes nothing", "[cmd][bench]") {
  auto const fx = make_fixture("dupuid");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  auto const dup =
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "grouped", "--base-sha", "x", "--config-hash", "y"});
  CHECK(dup.code == 6);
  CHECK(dup.err == "error: bench start: run_uid 'b1' already exists\n");

  // A MISSING plan is an FK failure the engine does not name — exit 1
  // `QueryFailed`, not a "no such plan" refusal. Reproduced, not improved.
  auto const bad_plan =
      dispatch(fx, {"bench", "start", "b9", "--plan", "999", "--arm", "strict", "--base-sha", "x", "--config-hash", "y"});
  CHECK(bad_plan.code == 1);
  CHECK(bad_plan.err == "error: bench start: QueryFailed\n");

  auto conn = open_db(fx);
  // The original row is UNTOUCHED — a duplicate must not have overwritten
  // its arm, and the failed one must not exist.
  CHECK(run_snapshot(conn) == "b1|1|strict|s|c|<NULL>|<NULL>|running|no-end");
}

TEST_CASE("bench show renders both wire formats and reports a missing uid", "[cmd][bench]") {
  auto const fx = make_fixture("show");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "deadbeef", "--config-hash", "ch1"})
          .code == 0);
  REQUIRE(dispatch(fx, {"bench", "event", "b1", "--kind", "result", "--seq", "1", "--payload", R"({"a":1})"}).code == 0);
  REQUIRE(dispatch(fx, {"bench", "touch", "b1", "--task", "1", "--path", "src/a.c", "--kind", "declared"}).code == 0);

  auto const text = dispatch(fx, {"bench", "show", "b1"});
  CHECK(text.code == 0);
  // Labels padded to 13; `corpus_repo` and `ended_at` OMITTED (not empty)
  // because both are unset; both section headers present with their counts.
  CHECK(text.out.starts_with("run:         b1\n"
                             "plan_id:     1\n"
                             "arm:         strict\n"
                             "status:      running\n"
                             "base_sha:    deadbeef\n"
                             "config_hash: ch1\n"
                             "started_at:  "));
  // The two sections do NOT share a row format: an event is `[<seq>] <kind>`
  // with the payload appended after a colon, a touch is `key=value` triples.
  CHECK(text.out.ends_with("\nevents (1):\n"
                           "  [1] result: {\"a\":1}\n"
                           "\ntouches (1):\n"
                           "  task=1 path=src/a.c kind=declared\n"));

  auto const json = dispatch(fx, {"bench", "show", "b1", "--json"});
  CHECK(json.code == 0);
  // `config_json` and `payload` are embedded VERBATIM as JSON values, not
  // escaped as strings — the reason both are validated at the parse layer.
  CHECK(json.out.starts_with(R"({"id":1,"run_uid":"b1","plan_id":1,"arm":"strict","base_sha":"deadbeef",)"
                             R"("config_hash":"ch1","config_json":null,"corpus_repo":null,"status":"running",)"));
  CHECK(json.out.find(R"("payload":{"a":1})") != std::string::npos);
  CHECK(json.out.find(R"("touches":[{"id":1,"task_id":1,"path":"src/a.c","kind":"declared")") != std::string::npos);

  for (auto const& args :
       {std::vector<std::string>{"bench", "show", "nosuch"}, std::vector<std::string>{"bench", "show", "nosuch", "--json"}}) {
    auto const missing = dispatch(fx, args);
    // The stderr line is identical in BOTH wire formats: the not-found path
    // never reaches a renderer, so `--json` changes nothing there. On
    // stdout, `--json` adds the additive error envelope (decision 1145,
    // task 6844); the text arm still writes nothing.
    CHECK(missing.code == 1);
    bool const wants_json = std::ranges::find(args, "--json") != args.end();
    if (wants_json) {
      CHECK(missing.out == planar::cmd::testsupport::json_error_envelope_line("bench show", "not_found"));
    } else {
      CHECK(missing.out.empty());
    }
    CHECK(missing.err == "error: bench show: run 'nosuch' not found\n");
  }
}

namespace {

/// @brief Run a shell line inside `dir`, aborting the test on failure.
/// FIXTURE construction only, never the behaviour under test — mirrors
/// `lib/git/git.t.cpp`'s `fixture_sh`.
auto harvest_fixture_sh(const std::filesystem::path& dir, std::string_view line) -> bool {
  std::string const composed = std::format("cd '{}' && {} >/dev/null 2>&1", dir.string(), line);
  return std::system(composed.c_str()) == 0;
}

auto harvest_init_repo(const std::filesystem::path& dir) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return harvest_fixture_sh(dir, "git init -q -b main") &&
         harvest_fixture_sh(dir, "git config user.email planar@example.invalid") &&
         harvest_fixture_sh(dir, "git config user.name Planar");
}

auto harvest_write_file(const std::filesystem::path& dir, std::string_view rel, std::string_view content) -> void {
  std::ofstream out(dir / rel);
  out << content;
}

auto harvest_commit_all(const std::filesystem::path& dir, std::string_view message) -> bool {
  return harvest_fixture_sh(dir, "git add -A") && harvest_fixture_sh(dir, std::format("git commit -q -m '{}'", message));
}

auto harvest_rev_parse_head(const std::filesystem::path& dir) -> std::string {
  std::string const cmd  = std::format("cd '{}' && git rev-parse HEAD", dir.string());
  std::FILE*        pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string           out;
  std::array<char, 128> buf{};
  std::size_t           n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
    out.append(buf.data(), n);
  }
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

auto have_git() -> bool {
  static bool const answer = std::system("git --version >/dev/null 2>&1") == 0;
  return answer;
}

} // namespace

TEST_CASE("bench harvest refuses --base without --head, and vice versa, before opening the database", "[cmd][bench]") {
  auto const fx = make_fixture("harvest_pairing");
  seed(fx);

  auto const base_only = dispatch(fx, {"bench", "harvest", "b1", "--task", "1", "--worktree", "/tmp", "--base", "abc"});
  CHECK(base_only.code == 2);
  CHECK(base_only.err == "error: bench harvest: --base and --head must be supplied together\n");
  CHECK_FALSE(base_only.db_open);

  auto const head_only = dispatch(fx, {"bench", "harvest", "b1", "--task", "1", "--worktree", "/tmp", "--head", "def"});
  CHECK(head_only.code == 2);
  CHECK(head_only.err == "error: bench harvest: --base and --head must be supplied together\n");
  CHECK_FALSE(head_only.db_open);
}

TEST_CASE("bench harvest reports a missing run", "[cmd][bench]") {
  auto const fx = make_fixture("harvest_missing");
  seed(fx);

  auto const missing = dispatch(fx, {"bench", "harvest", "nosuch", "--task", "1", "--worktree", "/tmp"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: bench harvest: run 'nosuch' not found\n");
}

TEST_CASE("bench harvest reports git_failed for a nonexistent worktree", "[cmd][bench]") {
  auto const fx = make_fixture("harvest_badworktree");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  auto const bad = dispatch(fx, {"bench", "harvest", "b1", "--task", "1", "--worktree", "/nonexistent/planar-harvest/xyzzy"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: bench harvest: git diff failed in worktree '/nonexistent/planar-harvest/xyzzy'\n");
}

TEST_CASE("bench harvest diffs the working tree, writes DISTINCT kind='actual' rows, and prints the count", "[cmd][bench]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  auto const fx = make_fixture("harvest_worktree");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  // A repo that is NOT the fixture's own `proj` (never diff the Planar
  // checkout itself) and NOT the real Planar checkout, per the task-6362
  // brief's hermeticity requirement.
  auto const repo = fx.root / "harvest-repo";
  REQUIRE(harvest_init_repo(repo));
  harvest_write_file(repo, "tracked.txt", "v1\n");
  REQUIRE(harvest_commit_all(repo, "base"));
  // One modified tracked file, one untracked (never-staged) new file --
  // both must land as ONE row each, proving the working-tree merge picks
  // up untracked files (task 4289's oracle fix) without duplicating.
  harvest_write_file(repo, "tracked.txt", "v2\n");
  harvest_write_file(repo, "untracked.txt", "brand new\n");

  auto const harvested = dispatch(fx, {"bench", "harvest", "b1", "--task", "1", "--worktree", repo.string()});
  CHECK(harvested.code == 0);
  CHECK(harvested.out == "2\n");

  auto conn = open_db(fx);
  CHECK(touch_rows(conn) == "b1|1|tracked.txt|actual\n"
                            "b1|1|untracked.txt|actual");
}

TEST_CASE("bench harvest diffs a base..head range across commits, no untracked-file pass", "[cmd][bench]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  auto const fx = make_fixture("harvest_range");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  auto const repo = fx.root / "harvest-range-repo";
  REQUIRE(harvest_init_repo(repo));
  harvest_write_file(repo, "a.txt", "a\n");
  REQUIRE(harvest_commit_all(repo, "base"));
  auto const base = harvest_rev_parse_head(repo);

  harvest_write_file(repo, "b.txt", "b\n");
  REQUIRE(harvest_commit_all(repo, "c1"));
  harvest_write_file(repo, "c.txt", "c\n");
  REQUIRE(harvest_commit_all(repo, "c2"));
  auto const head = harvest_rev_parse_head(repo);

  auto const harvested =
      dispatch(fx, {"bench", "harvest", "b1", "--task", "2", "--worktree", repo.string(), "--base", base, "--head", head});
  CHECK(harvested.code == 0);
  CHECK(harvested.out == "2\n");

  auto conn = open_db(fx);
  CHECK(touch_rows(conn) == "b1|2|b.txt|actual\n"
                            "b1|2|c.txt|actual");
}

TEST_CASE("bench harvest is idempotent: a second harvest of an unchanged diff does not duplicate rows", "[cmd][bench]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  auto const fx = make_fixture("harvest_idem");
  seed(fx);
  REQUIRE(
      dispatch(fx, {"bench", "start", "b1", "--plan", "1", "--arm", "strict", "--base-sha", "s", "--config-hash", "c"}).code ==
      0);

  auto const repo = fx.root / "harvest-idem-repo";
  REQUIRE(harvest_init_repo(repo));
  harvest_write_file(repo, "idem.txt", "v1\n");
  REQUIRE(harvest_commit_all(repo, "base"));
  harvest_write_file(repo, "idem.txt", "v2\n");

  CHECK(dispatch(fx, {"bench", "harvest", "b1", "--task", "1", "--worktree", repo.string()}).out == "1\n");
  // Re-harvesting the SAME diff must not fail and must not duplicate the row
  // -- the write goes through `touch_idempotent`, not `touch`.
  CHECK(dispatch(fx, {"bench", "harvest", "b1", "--task", "1", "--worktree", repo.string()}).out == "1\n");

  auto conn = open_db(fx);
  CHECK(touch_rows(conn) == "b1|1|idem.txt|actual");
}

TEST_CASE("run start mints a uid and prints JSON with or without --json", "[cmd][run]") {
  auto const fx = make_fixture("runstart");
  seed(fx);

  auto const bare = dispatch(fx, {"run", "start", "--plan", "1"});
  CHECK(bare.code == 0);
  CHECK(bare.out.starts_with(R"({"run_uid":")"));
  CHECK(bare.out.ends_with("\",\"plan_id\":1,\"arm\":\"op\"}\n"));

  auto const flagged = dispatch(fx, {"run", "start", "--plan", "1", "--json"});
  CHECK(flagged.code == 0);
  // `--json` is declared on this leaf and changes NOTHING but the uid.
  CHECK(flagged.out.ends_with("\",\"plan_id\":1,\"arm\":\"op\"}\n"));

  auto const workflow = dispatch(fx, {"run", "start", "--plan", "1", "--workflow", "wf"});
  CHECK(workflow.code == 0);
  CHECK(workflow.out.ends_with("\",\"plan_id\":1,\"arm\":\"wf\"}\n"));

  auto conn = open_db(fx);
  // The arm is `--workflow`'s value or the literal `op`, and an operational
  // run carries EMPTY STRINGS for base_sha/config_hash — `not null`
  // columns — while config_json/corpus_repo are genuinely NULL. That pair
  // of distinctions is invisible to `run show`, which prints neither.
  CHECK(query(conn, R"(select arm, base_sha, config_hash, coalesce(config_json,'<NULL>'),
                         coalesce(corpus_repo,'<NULL>'), status from runs order by id)",
              6) == "op|||<NULL>|<NULL>|running\n"
                    "op|||<NULL>|<NULL>|running\n"
                    "wf|||<NULL>|<NULL>|running");
  // 32 lowercase hex characters, distinct per run.
  auto const uids = query(conn, "select run_uid from runs order by id", 1);
  CHECK(query(conn, "select count(distinct run_uid) from runs", 1) == "3");
  CHECK(query(conn, "select count(*) from runs where length(run_uid) = 32 and run_uid glob '[0-9a-f]*'", 1) == "3");
  CHECK(uids.find('|') == std::string::npos);
}

TEST_CASE("run event AUTO-INCREMENTS the seq, unlike bench event", "[cmd][run]") {
  auto const fx = make_fixture("runevent");
  seed(fx);
  auto const started = dispatch(fx, {"run", "start", "--plan", "1"});
  REQUIRE(started.code == 0);
  auto const uid = started.out.substr(std::string_view{R"({"run_uid":")"}.size(), 32);

  auto const first = dispatch(fx, {"run", "event", uid, "--kind", "step"});
  CHECK(first.code == 0);
  CHECK(first.out == std::format(R"({{"run_uid":"{}","seq":1,"kind":"step"}})"
                                 "\n",
                                 uid));
  auto const second = dispatch(fx, {"run", "event", uid, "--kind", "step2", "--payload", R"({"a":1})", "--json"});
  CHECK(second.code == 0);
  CHECK(second.out == std::format(R"({{"run_uid":"{}","seq":2,"kind":"step2"}})"
                                  "\n",
                                  uid));

  auto const bad = dispatch(fx, {"run", "event", uid, "--kind", "k", "--payload", "notjson"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: run event: --payload is not valid JSON: notjson\n");

  auto const missing = dispatch(fx, {"run", "event", "nosuchuid", "--kind", "k"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: run event: run 'nosuchuid' not found\n");

  auto conn = open_db(fx);
  // Seqs 1 and 2, assigned by the handler rather than supplied. The
  // rejected payload wrote no third row.
  CHECK(query(conn, "select cast(seq as text), kind, coalesce(payload,'<NULL>') from run_events order by id", 3) ==
        "1|step|<NULL>\n"
        "2|step2|{\"a\":1}");
}

TEST_CASE("run finish and run show, including the post-finish append", "[cmd][run]") {
  auto const fx = make_fixture("runfinish");
  seed(fx);
  auto const started = dispatch(fx, {"run", "start", "--plan", "1"});
  REQUIRE(started.code == 0);
  auto const uid = started.out.substr(std::string_view{R"({"run_uid":")"}.size(), 32);
  REQUIRE(dispatch(fx, {"run", "event", uid, "--kind", "step"}).code == 0);

  auto const text = dispatch(fx, {"run", "show", uid});
  CHECK(text.code == 0);
  // Labels padded to 12 here, 13 in `bench show` — the two renderers are
  // NOT one parameterized function, and `run show` carries no touches
  // section at all.
  CHECK(text.out.starts_with(std::format("run:        {}\nplan_id:    1\narm:        op\nstatus:     running\n", uid)));
  CHECK(text.out.ends_with("\nevents (1):\n  [1] step\n"));
  CHECK(text.out.find("touches") == std::string::npos);

  auto const done = dispatch(fx, {"run", "finish", uid, "--status", "completed"});
  CHECK(done.code == 0);
  CHECK(done.out == std::format(R"({{"run_uid":"{}","status":"completed"}})"
                                "\n",
                                uid));

  // UNGUARDED, like `bench`: an event after the finish appends at seq 2,
  // and a second finish replaces the terminal status.
  CHECK(dispatch(fx, {"run", "event", uid, "--kind", "after-finish"}).code == 0);
  CHECK(dispatch(fx, {"run", "finish", uid, "--status", "error"}).code == 0);

  auto const bad = dispatch(fx, {"run", "finish", uid, "--status", "bogus"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: run finish: invalid --status 'bogus'; expected completed, aborted, or error\n");
  CHECK_FALSE(bad.db_open);

  auto const missing = dispatch(fx, {"run", "finish", "nosuchuid", "--status", "completed"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: run finish: run 'nosuchuid' not found\n");

  auto conn = open_db(fx);
  CHECK(query(conn, "select status, case when ended_at is null then 'no-end' else 'ended' end from runs", 2) == "error|ended");
  CHECK(query(conn, "select cast(seq as text), kind from run_events order by id", 2) == "1|step\n"
                                                                                        "2|after-finish");

  auto const json = dispatch(fx, {"run", "show", uid, "--json"});
  CHECK(json.code == 0);
  // No base_sha / config_hash / config_json / corpus_repo / touches keys —
  // the operational view deliberately omits every measurement field, even
  // though the underlying row carries them.
  for (auto const* absent : {"base_sha", "config_hash", "config_json", "corpus_repo", "touches"}) {
    CHECK(json.out.find(absent) == std::string::npos);
  }
  CHECK(json.out.find(R"("status":"error")") != std::string::npos);

  auto const gone = dispatch(fx, {"run", "show", "nosuchuid", "--json"});
  CHECK(gone.code == 1);
  CHECK(gone.err == "error: run show: run 'nosuchuid' not found\n");
}
