// @file plan_strategy_leaves.t.cpp
// @brief In-process tests for the TWO leaves plan 996 task 6310 landed
// together: `planar plan recommend-strategy` and `planar plan divergence`.
//
// ## THEY SHARE A LOADER AND AGREE ON ALMOST NOTHING ELSE
//
// Task 6298 paired these because they share ~300 lines of loader substrate in
// the oracle's `strategy.zig`. The pairing is right about the loader and
// misleading about everything downstream, so every case below probes the two
// verbs INDEPENDENTLY rather than asserting one from the other. The
// disagreements this file pins, all oracle-measured:
//
//   * `divergence` runs NO unilateral rule. On the seven-task plan where
//     `recommend-strategy` serializes five tasks (one per rule), `divergence`
//     reports `declared_overlaps:0 derived_overlaps:0 flips:0`. It applies
//     only rule 2's pairwise overlap test.
//   * An EMPTY touch set means opposite things. Under `recommend-strategy` it
//     is "touches everything" and drops the task via rule 2; under
//     `divergence` it simply never conflicts and contributes no overlap.
//   * `--closure-source` exists ONLY on `recommend-strategy`. On `divergence`
//     it is a parse-time refusal, and `divergence`'s JSON carries no
//     `closure_source` field.
//   * `jaccard` renders TWO ways: shortest-round-trip in JSON (`0`, `0.5`,
//     `0.3333333333333333`) and fixed four-decimal in text (`0.0000`,
//     `0.5000`, `0.3333`). A single formatter would be wrong in one arm.
//
// They DO agree on the candidate set (`status = 'todo'`, ordered
// `priority, id`) and on both refusal arms, and those agreements are pinned
// too — asserting only the disagreements would leave the shared half
// untested.
//
// ## THE FIRST CASE ASSERTS THE FIXTURE BEFORE ANYTHING IS COMPARED
//
// Every other case is a byte comparison, and a fixture that seeded the wrong
// rows would satisfy most of them while exercising one arm of six. The
// oracle probe written alongside this file failed exactly that way on its
// first run: `plan create` refused with `project has no association` (the
// arena had `init` but no `assoc create`/`assoc add`), every plan id came
// back empty, and the run compared nothing at all. The guard that caught it
// also had to be fixed — `exit 1` inside `$(...)` kills only the subshell, so
// the script sailed past its own failed seed. So `seed` below asserts its own
// post-state and the first case pins that post-state again.
//
// An absence assertion is always paired with its presence: `flips == 0` on
// the transitive-only plan is only meaningful next to `flips == 1` on the
// plan where the shared symbol is `reference`.
//
// ## ORACLE PROVENANCE
//
// Every expected byte was captured from `zig/zig-out/bin/planar` built at
// this cycle's base, in pinned scratch arenas (`PLANAR_DB` under a temp root
// — never the operator's database). Exit codes were read from the command
// itself, never through a pipe.
//
// The captures were replayed as a 71-case differential against the built C++
// binary in a second identically-seeded arena. 67 agreed on stdout, stderr
// and exit code. The four that differed are ALL parser-layer refusals
// (missing positional, unknown flag, extra positional) where CLI11 and the
// oracle's etcli-zig word the message differently at the SAME exit code 2.
// That divergence is systemic and pre-existing, not introduced here: probed
// on `plan descendants`, `plan next`, `plan show` and `plan list` — leaves
// this task never touched — it reproduces identically on every one. Those
// four are therefore NOT pinned as byte comparisons below; the leaf-owned
// half of each refusal (the exit code) is.
//
// The captures that decided a shape:
//
//   $Z plan recommend-strategy <p> --json  -> `serialized[].excluded_by` is in
//       RULE-APPLICATION order, not sorted: 1, 5, 6, 2-empty, 3, 4, then the
//       rule-2 overlap pass. A task tripping five rules emitted
//       `[1, 5, 6, 3, 4]`.
//
//   $Z plan recommend-strategy <p> --json  -> both sides of an overlapping
//       pair name the SAME touch, because `shared_touch` returns the touch
//       from its first argument and one description is written into both. A
//       path-vs-whole-repo conflict reports the PATH on both tasks.
//
//   $Z plan divergence <p> --json          -> `jaccard` is `0` (not `0.0`)
//       when the union is empty, and `0.3333333333333333` at 1/3.

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
                         std::format("planar_strategy_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Run raw SQL against the fixture database.
/// @param fx The fixture.
/// @param sql The statement.
auto exec_sql(const fixture& fx, std::string_view sql) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto ok = conn->execute(sql);
  REQUIRE(ok.has_value());
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

/// @brief Seed the shared arena, asserting every step.
///
/// Guarded step by step on purpose: the oracle probe this mirrors produced a
/// vacuous run because `plan create` refused for a missing association and
/// nothing checked. Closures are inserted as SQL because seeding them through
/// `closure compute` would require running the real extractor over real
/// files, which is a different subsystem entirely.
/// @param fx The fixture to seed.
auto seed(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--allow-no-repo", "--name", "t6310"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "t6310", "--kind", "project"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "t6310", (fx.root / "proj").string()}).code == 0);

  // plan 1: empty. plan 2: two tasks with NO touches at all.
  REQUIRE(dispatch(fx, {"plan", "create", "P1 empty"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "P2 bare"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "bare A", "--plan", "2"}).code == 0); // task 1
  REQUIRE(dispatch(fx, {"task", "add", "bare B", "--plan", "2"}).code == 0); // task 2

  // plan 3: the D4 flip -- declared-disjoint, derived-overlapping.
  REQUIRE(dispatch(fx, {"plan", "create", "P3 flip"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "flip A", "--plan", "3", "--slug", "t6310-flip-a"}).code == 0); // 3
  REQUIRE(dispatch(fx, {"task", "add", "flip B", "--plan", "3"}).code == 0);                           // 4
  auto const repo = scalar(fx, "select id from projects limit 1");
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (3,{},'src/foo.zig')", repo));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (4,{},'src/bar.zig')", repo));
  for (auto const& [tid, path, sym, role] :
       std::vector<std::tuple<int, std::string, std::string, std::string>>{{3, "src/foo.zig", "foo.run", "modify"},
                                                                           {3, "src/shared.zig", "shared.helper", "reference"},
                                                                           {4, "src/bar.zig", "bar.run", "modify"},
                                                                           {4, "src/shared.zig", "shared.helper", "reference"}}) {
    exec_sql(fx, std::format("insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) "
                             "values ({},{},'{}','{}','{}',0,'t')",
                             tid, repo, path, sym, role));
  }

  // plan 4: one task per unilateral rule, plus a task tripping FIVE of them.
  REQUIRE(dispatch(fx, {"plan", "create", "P4 rules"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "ok", "--plan", "4"}).code == 0);      // 5
  REQUIRE(dispatch(fx, {"task", "add", "multi", "--plan", "4"}).code == 0);   // 6
  REQUIRE(dispatch(fx, {"task", "add", "blocker", "--plan", "4"}).code == 0); // 7
  REQUIRE(dispatch(fx, {"task", "add", "peer", "--plan", "4"}).code == 0);    // 8
  exec_sql(fx, "insert into projects (slug,name,root_path) values ('t6310r2','t6310r2','/tmp/t6310r2')");
  auto const repo2 = scalar(fx, "select id from projects where slug='t6310r2'");
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (5,{},'src/ok.zig')", repo2));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (6,{},'migrations/00001_a.sql')", repo2));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (6,{},'AGENTS.md')", repo2));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (6,{},'src/sh.zig')", repo2));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (8,{},'src/sh.zig')", repo2));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (7,{},'src/blk.zig')", repo2));
  exec_sql(fx, "insert into questions (scope_kind,title,status) values ('global','Q','open')");
  auto const qid = scalar(fx, "select id from questions where title='Q'");
  exec_sql(fx, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                           "values ('task',6,'question',{},'addresses')",
                           qid));
  exec_sql(fx, "insert into decisions (scope_kind,title,body,status) values ('global','D','b','proposed')");
  auto const did = scalar(fx, "select id from decisions where title='D'");
  exec_sql(fx, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                           "values ('decision',{},'task',6,'depends-on')",
                           did));
  exec_sql(fx, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
               "values ('task',6,'task',7,'depends-on')");

  // plan 5: a path touch against a whole-repo touch on the SAME repo.
  REQUIRE(dispatch(fx, {"plan", "create", "P5 whole"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "path", "--plan", "5"}).code == 0);  // 9
  REQUIRE(dispatch(fx, {"task", "add", "whole", "--plan", "5"}).code == 0); // 10
  exec_sql(fx, "insert into projects (slug,name,root_path) values ('t6310r3','t6310r3','/tmp/t6310r3')");
  auto const repo3 = scalar(fx, "select id from projects where slug='t6310r3'");
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (9,{},'src/foo.zig')", repo3));
  exec_sql(fx, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                           "values ('task',10,'repo',{},'touches')",
                           repo3));

  // plan 6: union of 3 overlapping pairs with exactly 1 flip -> jaccard 1/3.
  REQUIRE(dispatch(fx, {"plan", "create", "P6 frac"}).code == 0);
  exec_sql(fx, "insert into projects (slug,name,root_path) values ('t6310r4','t6310r4','/tmp/t6310r4')");
  auto const repo4 = scalar(fx, "select id from projects where slug='t6310r4'");
  for (int i = 1; i <= 4; ++i) {
    REQUIRE(dispatch(fx, {"task", "add", std::format("F{}", i), "--plan", "6"}).code == 0); // 11..14
  }
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (11,{},'p.zig')", repo4));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (12,{},'p.zig')", repo4));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (13,{},'q.zig')", repo4));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (14,{},'q.zig')", repo4));
  for (auto const& [tid, sym, role] : std::vector<std::tuple<int, std::string, std::string>>{{11, "S1", "modify"},
                                                                                             {12, "S1", "modify"},
                                                                                             {13, "S2", "modify"},
                                                                                             {14, "S2", "modify"},
                                                                                             {11, "S3", "reference"},
                                                                                             {13, "S3", "reference"}}) {
    exec_sql(fx, std::format("insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) "
                             "values ({},{},'x.zig','{}','{}',0,'t')",
                             tid, repo4, sym, role));
  }

  // plan 7: the ONLY shared symbol is `transitive` -- excluded, so no flip.
  REQUIRE(dispatch(fx, {"plan", "create", "P7 trans"}).code == 0);
  exec_sql(fx, "insert into projects (slug,name,root_path) values ('t6310r5','t6310r5','/tmp/t6310r5')");
  auto const repo5 = scalar(fx, "select id from projects where slug='t6310r5'");
  REQUIRE(dispatch(fx, {"task", "add", "TR1", "--plan", "7"}).code == 0); // 15
  REQUIRE(dispatch(fx, {"task", "add", "TR2", "--plan", "7"}).code == 0); // 16
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (15,{},'a.zig')", repo5));
  exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values (16,{},'b.zig')", repo5));
  exec_sql(fx, std::format("insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) "
                           "values (15,{},'d.zig','DEEP','transitive',0,'t')",
                           repo5));
  exec_sql(fx, std::format("insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) "
                           "values (16,{},'d.zig','DEEP','transitive',0,'t')",
                           repo5));

  // plan 8: a `doing` task blocks a `todo` one. This separates the CANDIDATE
  // set (`status='todo'`) from the BLOCKER set (`not in ('done','cancelled')`)
  // -- a port that used one query for both passes every other case here.
  REQUIRE(dispatch(fx, {"plan", "create", "P8 status"}).code == 0);
  exec_sql(fx, "insert into projects (slug,name,root_path) values ('t6310r6','t6310r6','/tmp/t6310r6')");
  auto const repo6 = scalar(fx, "select id from projects where slug='t6310r6'");
  REQUIRE(dispatch(fx, {"task", "add", "S todo", "--plan", "8"}).code == 0);  // 17
  REQUIRE(dispatch(fx, {"task", "add", "S doing", "--plan", "8"}).code == 0); // 18
  REQUIRE(dispatch(fx, {"task", "add", "S done", "--plan", "8"}).code == 0);  // 19
  REQUIRE(dispatch(fx, {"task", "add", "S dep", "--plan", "8"}).code == 0);   // 20
  for (int i = 17; i <= 20; ++i) {
    exec_sql(fx, std::format("insert into task_touch_paths (task_id,repo_id,path) values ({},{},'f{}.zig')", i, repo6, i));
  }
  exec_sql(fx, "update tasks set status='doing' where id=18");
  exec_sql(fx, "update tasks set status='done' where id=19");
  exec_sql(fx, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
               "values ('task',20,'task',18,'depends-on')");
}

} // namespace

TEST_CASE("the strategy fixture reaches every rule and both sources before anything is compared",
          "[cmd][plan][strategy][fixture]") {
  auto const fx = make_fixture("shape");
  seed(fx);

  CHECK(scalar(fx, "select count(*) from plans") == 8);
  CHECK(scalar(fx, "select count(*) from tasks") == 20);
  CHECK(scalar(fx, "select count(*) from closures") == 12);

  // The candidate/blocker split only exists if the statuses actually landed.
  CHECK(scalar(fx, "select count(*) from tasks where status='doing'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where status='done'") == 1);
  CHECK(scalar(fx, "select count(*) from tasks where plan_id=8 and status='todo'") == 2);

  // Each unilateral rule needs its own trigger present, not merely absent.
  CHECK(scalar(fx, "select count(*) from task_touch_paths where path like 'migrations/%'") == 1);
  CHECK(scalar(fx, "select count(*) from task_touch_paths where path='AGENTS.md'") == 1);
  CHECK(scalar(fx, "select count(*) from questions where status='open'") == 1);
  CHECK(scalar(fx, "select count(*) from decisions where status='proposed'") == 1);
  CHECK(scalar(fx, "select count(*) from entity_links where relationship='depends-on' and from_kind='task'") == 2);
  CHECK(scalar(fx, "select count(*) from entity_links where to_kind='repo' and relationship='touches'") == 1);

  // Both closure roles are represented; a fixture with only `transitive` rows
  // would make every derived-overlap assertion below vacuously true.
  CHECK(scalar(fx, "select count(*) from closures where role='transitive'") == 2);
  CHECK(scalar(fx, "select count(*) from closures where role in ('modify','reference')") == 10);
}

TEST_CASE("an empty plan gives recommend-strategy empty ARRAYS and divergence all zeroes", "[cmd][plan][strategy][empty]") {
  auto const fx = make_fixture("empty");
  seed(fx);

  auto const rec = dispatch(fx, {"plan", "recommend-strategy", "1", "--json"});
  CHECK(rec.code == 0);
  CHECK(rec.out == R"({"plan_id":1,"closure_source":"declared","parallel_eligible":[],"serialized":[],)"
                   R"("summary":{"open_tasks":0,"eligible":0,"serialized":0,"fan_out_available":false},)"
                   R"("recommended_note":"no fan-out: no eligible tasks; run sequentially"})"
                   "\n");

  auto const div = dispatch(fx, {"plan", "divergence", "1", "--json"});
  CHECK(div.code == 0);
  // `jaccard` is `0`, NOT `0.0` -- the JSON arm is shortest-round-trip.
  CHECK(div.out == R"({"plan_id":1,"open_tasks":0,"pairs":0,"declared_overlaps":0,)"
                   R"("derived_overlaps":0,"flips":0,"jaccard":0})"
                   "\n");

  auto const rec_text = dispatch(fx, {"plan", "recommend-strategy", "1"});
  CHECK(rec_text.code == 0);
  CHECK(rec_text.out == "plan:1  source:declared  open:0  eligible:0  serialized:0  fan_out_available:no\n"
                        "  no fan-out: no eligible tasks; run sequentially\n"
                        "parallel-eligible:\n  (none)\nserialized:\n  (none)\n");

  auto const div_text = dispatch(fx, {"plan", "divergence", "1"});
  CHECK(div_text.code == 0);
  // ...and `0.0000` in the text arm, from the same zero.
  CHECK(div_text.out == "plan:1  open:0  pairs:0  declared_overlaps:0  derived_overlaps:0  flips:0  jaccard:0.0000\n");
}

TEST_CASE("an empty touch set drops a task under recommend-strategy and is inert under divergence",
          "[cmd][plan][strategy][empty-touches]") {
  auto const fx = make_fixture("baretouch");
  seed(fx);

  // recommend-strategy: rule 2 treats "no touches" as "touches everything".
  auto const rec = dispatch(fx, {"plan", "recommend-strategy", "2", "--json"});
  CHECK(rec.code == 0);
  CHECK(rec.out == R"({"plan_id":2,"closure_source":"declared","parallel_eligible":[],"serialized":[)"
                   R"({"id":1,"slug":null,"title":"bare A","excluded_by":[{"rule":2,)"
                   // Custom delimiter: the reason text itself ends `...-everything)`,
                   // and the default `)"` terminator would close the literal there.
                   R"J("reason":"excluded by rule 2: no task_touches declared (treated as touches-everything)"}]},)J"
                   R"({"id":2,"slug":null,"title":"bare B","excluded_by":[{"rule":2,)"
                   R"J("reason":"excluded by rule 2: no task_touches declared (treated as touches-everything)"}]}],)J"
                   R"("summary":{"open_tasks":2,"eligible":0,"serialized":2,"fan_out_available":false},)"
                   R"("recommended_note":"no fan-out: no eligible tasks; run sequentially"})"
                   "\n");

  // divergence: the SAME two tasks simply never conflict. It counts the pair
  // and reports no overlap -- it has no touches-everything concept at all.
  auto const div = dispatch(fx, {"plan", "divergence", "2", "--json"});
  CHECK(div.code == 0);
  CHECK(div.out == R"({"plan_id":2,"open_tasks":2,"pairs":1,"declared_overlaps":0,)"
                   R"("derived_overlaps":0,"flips":0,"jaccard":0})"
                   "\n");
}

TEST_CASE("the closure source flips rule 2 for recommend-strategy and is what divergence measures", "[cmd][plan][strategy][d4]") {
  auto const fx = make_fixture("flip");
  seed(fx);

  // declared: different files -> both eligible, fan-out available.
  auto const declared = dispatch(fx, {"plan", "recommend-strategy", "3", "--json"});
  CHECK(declared.code == 0);
  CHECK(declared.out == R"({"plan_id":3,"closure_source":"declared","parallel_eligible":[)"
                        R"({"id":3,"slug":"t6310-flip-a","title":"flip A"},)"
                        R"({"id":4,"slug":null,"title":"flip B"}],"serialized":[],)"
                        R"("summary":{"open_tasks":2,"eligible":2,"serialized":0,"fan_out_available":true},)"
                        R"("recommended_note":"parallel-fanout available: 2 eligible tasks"})"
                        "\n");

  // derived: the shared `reference` symbol serializes both. Note BOTH sides
  // name the same symbol.
  auto const derived = dispatch(fx, {"plan", "recommend-strategy", "3", "--closure-source", "derived", "--json"});
  CHECK(derived.code == 0);
  CHECK(derived.out == R"({"plan_id":3,"closure_source":"derived","parallel_eligible":[],"serialized":[)"
                       R"({"id":3,"slug":"t6310-flip-a","title":"flip A","excluded_by":[{"rule":2,)"
                       R"("reason":"excluded by rule 2: overlaps task 4 on shared.helper"}]},)"
                       R"({"id":4,"slug":null,"title":"flip B","excluded_by":[{"rule":2,)"
                       R"("reason":"excluded by rule 2: overlaps task 3 on shared.helper"}]}],)"
                       R"("summary":{"open_tasks":2,"eligible":0,"serialized":2,"fan_out_available":false},)"
                       R"("recommended_note":"no fan-out: no eligible tasks; run sequentially"})"
                       "\n");

  // An explicit `declared` is byte-identical to the default.
  auto const explicit_declared = dispatch(fx, {"plan", "recommend-strategy", "3", "--closure-source", "declared", "--json"});
  CHECK(explicit_declared.code == 0);
  CHECK(explicit_declared.out == declared.out);

  // divergence sees exactly that one pair flip. Jaccard is 1.0 -- rendered
  // `1` in JSON, `1.0000` in text.
  auto const div = dispatch(fx, {"plan", "divergence", "3", "--json"});
  CHECK(div.code == 0);
  CHECK(div.out == R"({"plan_id":3,"open_tasks":2,"pairs":1,"declared_overlaps":0,)"
                   R"("derived_overlaps":1,"flips":1,"jaccard":1})"
                   "\n");
  auto const div_text = dispatch(fx, {"plan", "divergence", "3"});
  CHECK(div_text.code == 0);
  CHECK(div_text.out == "plan:3  open:2  pairs:1  declared_overlaps:0  derived_overlaps:1  flips:1  jaccard:1.0000\n");
}

TEST_CASE("every unilateral rule fires, and their reasons appear in rule-application order", "[cmd][plan][strategy][rules]") {
  auto const fx = make_fixture("rules");
  seed(fx);

  auto const rec = dispatch(fx, {"plan", "recommend-strategy", "4", "--json"});
  CHECK(rec.code == 0);

  // Task 6 trips rules 1, 5, 6, 3 and 4 -- emitted in APPLICATION order, so
  // `[1, 5, 6, 3, 4]` and NOT ascending `[1, 3, 4, 5, 6]`.
  CHECK(rec.out.contains(R"({"id":6,"slug":null,"title":"multi","excluded_by":[)"
                         R"({"rule":1,"reason":"excluded by rule 1: blocked_by not-done task 7"},)"
                         R"({"rule":5,"reason":"excluded by rule 5: linked to an open question"},)"
                         R"({"rule":6,"reason":"excluded by rule 6: linked to a proposed decision"},)"
                         R"({"rule":3,"reason":"excluded by rule 3: touches migrations/00001_a.sql"},)"
                         R"({"rule":4,"reason":"excluded by rule 4: touches singleton authoritative file AGENTS.md"}]})"));

  // Task 8's ONLY conflict was with task 6 on `src/sh.zig`, and task 6 was
  // already dropped by the unilateral rules -- so the rule-2 pass skipped the
  // pair and task 8 stays eligible. This is the non-cascade invariant.
  CHECK(rec.out.contains(R"("parallel_eligible":[{"id":5,"slug":null,"title":"ok"},)"
                         R"({"id":7,"slug":null,"title":"blocker"},{"id":8,"slug":null,"title":"peer"}])"));
  CHECK(rec.out.contains(R"("summary":{"open_tasks":4,"eligible":3,"serialized":1,"fan_out_available":true})"));

  // divergence over the SAME plan runs none of that. Four open tasks, six
  // pairs, and the only declared overlap is 6-vs-8 on `src/sh.zig` -- task 6
  // is NOT dropped here, because divergence drops nothing.
  auto const div = dispatch(fx, {"plan", "divergence", "4", "--json"});
  CHECK(div.code == 0);
  CHECK(div.out == R"({"plan_id":4,"open_tasks":4,"pairs":6,"declared_overlaps":1,)"
                   R"("derived_overlaps":0,"flips":1,"jaccard":1})"
                   "\n");
}

TEST_CASE("a whole-repo touch conflicts with a same-repo path touch and both sides name the PATH",
          "[cmd][plan][strategy][whole-repo]") {
  auto const fx = make_fixture("whole");
  seed(fx);

  auto const rec = dispatch(fx, {"plan", "recommend-strategy", "5", "--json"});
  CHECK(rec.code == 0);
  // `shared_touch` returns the touch from its FIRST argument and one
  // description is written into both reasons, so the whole-repo side reports
  // `src/foo.zig` rather than `repo:N (whole repo)`.
  CHECK(rec.out.contains(R"({"id":9,"slug":null,"title":"path","excluded_by":[{"rule":2,)"
                         R"("reason":"excluded by rule 2: overlaps task 10 on src/foo.zig"}]})"));
  CHECK(rec.out.contains(R"({"id":10,"slug":null,"title":"whole","excluded_by":[{"rule":2,)"
                         R"("reason":"excluded by rule 2: overlaps task 9 on src/foo.zig"}]})"));
  CHECK(rec.out.contains(R"("fan_out_available":false)"));
}

TEST_CASE("jaccard renders shortest-round-trip in JSON and fixed four-decimal in text", "[cmd][plan][strategy][jaccard]") {
  auto const fx = make_fixture("frac");
  seed(fx);

  // Three overlapping pairs in the union, exactly one flip -> 1/3.
  auto const div = dispatch(fx, {"plan", "divergence", "6", "--json"});
  CHECK(div.code == 0);
  CHECK(div.out == R"({"plan_id":6,"open_tasks":4,"pairs":6,"declared_overlaps":2,)"
                   R"("derived_overlaps":3,"flips":1,"jaccard":0.3333333333333333})"
                   "\n");

  auto const div_text = dispatch(fx, {"plan", "divergence", "6"});
  CHECK(div_text.code == 0);
  CHECK(div_text.out == "plan:6  open:4  pairs:6  declared_overlaps:2  derived_overlaps:3  flips:1  jaccard:0.3333\n");
}

TEST_CASE("a transitive-only shared symbol is excluded from the derived set", "[cmd][plan][strategy][transitive]") {
  auto const fx = make_fixture("trans");
  seed(fx);

  // Paired with the flip case above, which shares a `reference` symbol and
  // DOES flip. Without that pairing this zero could mean the fixture simply
  // seeded no closures.
  auto const div = dispatch(fx, {"plan", "divergence", "7", "--json"});
  CHECK(div.code == 0);
  CHECK(div.out == R"({"plan_id":7,"open_tasks":2,"pairs":1,"declared_overlaps":0,)"
                   R"("derived_overlaps":0,"flips":0,"jaccard":0})"
                   "\n");

  // ...and rule 2 sees no overlap under `derived`, so both stay eligible.
  auto const rec = dispatch(fx, {"plan", "recommend-strategy", "7", "--closure-source", "derived", "--json"});
  CHECK(rec.code == 0);
  CHECK(rec.out.contains(R"("summary":{"open_tasks":2,"eligible":2,"serialized":0,"fan_out_available":true})"));
}

TEST_CASE("the todo candidate set and the not-done blocker set are different queries", "[cmd][plan][strategy][status]") {
  auto const fx = make_fixture("status");
  seed(fx);

  auto const rec = dispatch(fx, {"plan", "recommend-strategy", "8", "--json"});
  CHECK(rec.code == 0);
  // Only tasks 17 and 20 are candidates: 18 is `doing`, 19 is `done`.
  CHECK(rec.out.contains(R"("summary":{"open_tasks":2,)"));
  // Task 20 depends on task 18, which is `doing` -- NOT a candidate, but very
  // much "not done", so rule 1 still fires. A port that reused the `todo`
  // query for the blocker set would leave task 20 eligible here.
  CHECK(rec.out.contains(R"({"rule":1,"reason":"excluded by rule 1: blocked_by not-done task 18"})"));
  CHECK(rec.out.contains(R"("parallel_eligible":[{"id":17,"slug":null,"title":"S todo"}])"));
  CHECK(rec.out.contains(R"("recommended_note":"no fan-out: only 1 eligible task; run sequentially")"));

  // divergence counts only the two `todo` tasks -> exactly one pair.
  auto const div = dispatch(fx, {"plan", "divergence", "8", "--json"});
  CHECK(div.code == 0);
  CHECK(div.out.contains(R"("open_tasks":2,"pairs":1,)"));
}

TEST_CASE("both leaves refuse a missing plan at exit 1 and a non-integer id at exit 2", "[cmd][plan][strategy][refusal]") {
  auto const fx = make_fixture("refuse");
  seed(fx);

  // This is the half the two verbs genuinely SHARE, so it is asserted on
  // both rather than on one and assumed for the other.
  for (auto const& verb : {"recommend-strategy", "divergence"}) {
    INFO("verb: " << verb);

    auto const missing = dispatch(fx, {"plan", verb, "99999", "--json"});
    CHECK(missing.code == 1); // not_found maps to exit 1, not 2
    CHECK(missing.err == "error: plan 99999 not found\n");
    // The refusal is NOT a JSON document -- only the additive envelope
    // (decision 1145, task 6844) is on stdout.
    CHECK(missing.out == planar::cmd::testsupport::json_error_envelope_line(std::format("plan {}", verb), "not_found"));

    auto const missing_text = dispatch(fx, {"plan", verb, "99999"});
    CHECK(missing_text.code == 1);
    CHECK(missing_text.err == "error: plan 99999 not found\n");

    auto const nonint = dispatch(fx, {"plan", verb, "abc", "--json"});
    CHECK(nonint.code == 2);
    CHECK(nonint.err == "error: plan id must be an integer, got 'abc'\n");

    // An id too large for int64 is a PARSE failure, not a lookup failure.
    auto const huge = dispatch(fx, {"plan", verb, "99999999999999999999", "--json"});
    CHECK(huge.code == 2);
    CHECK(huge.err == "error: plan id must be an integer, got '99999999999999999999'\n");

    // Plan 0 exists nowhere but parses fine, so it is a lookup failure.
    auto const zero = dispatch(fx, {"plan", verb, "0", "--json"});
    CHECK(zero.code == 1);
    CHECK(zero.err == "error: plan 0 not found\n");
  }
}

TEST_CASE("closure-source is validated before the database and exists on only one of the two leaves",
          "[cmd][plan][strategy][refusal][flag]") {
  auto const fx = make_fixture("source");
  seed(fx);

  auto const bogus = dispatch(fx, {"plan", "recommend-strategy", "3", "--closure-source", "bogus", "--json"});
  CHECK(bogus.code == 2);
  CHECK(bogus.err == "error: --closure-source must be 'declared' or 'derived', got 'bogus'\n");

  auto const empty = dispatch(fx, {"plan", "recommend-strategy", "3", "--closure-source", "", "--json"});
  CHECK(empty.code == 2);
  CHECK(empty.err == "error: --closure-source must be 'declared' or 'derived', got ''\n");

  // The flag is checked BEFORE the plan is looked up: a bad source on a
  // nonexistent plan reports the FLAG, at exit 2, not the plan at exit 1.
  auto const both_wrong = dispatch(fx, {"plan", "recommend-strategy", "99999", "--closure-source", "bogus", "--json"});
  CHECK(both_wrong.code == 2);
  CHECK(both_wrong.err == "error: --closure-source must be 'declared' or 'derived', got 'bogus'\n");

  // `divergence` does not declare the flag at all, so this dies in the
  // PARSER rather than in the handler. The wording is deliberately NOT
  // pinned: CLI11 says `The following arguments were not expected` where the
  // oracle's etcli-zig says `unknown flag (got --closure-source)`, and that
  // gap is systemic across every ported leaf (reproduced on `plan
  // descendants`, `plan next`, `plan show` and `plan list`, none of which
  // this task touched) rather than anything introduced here.
  //
  // What IS pinned is the part that agrees and that this leaf owns: the exit
  // code, and the stream split. Decision 1004 (task 6271) moved every
  // parse-failure diagnostic to stderr, matching the handler-level refusals
  // above -- stdout stays empty on a parser refusal now too. This block used
  // to pin the opposite split (message on stdout, kind on stderr) as
  // load-bearing measured behavior; that shape predates the decision.
  auto const unknown = dispatch(fx, {"plan", "divergence", "3", "--closure-source", "derived", "--json"});
  CHECK(unknown.code == 2);
  CHECK(unknown.out.empty());
  CHECK(unknown.err.contains("error:"));
  // The flag really is absent from this leaf and present on the sibling --
  // asserting the refusal alone would pass even if BOTH leaves rejected it.
  auto const accepted = dispatch(fx, {"plan", "recommend-strategy", "3", "--closure-source", "derived", "--json"});
  CHECK(accepted.code == 0);
  CHECK(accepted.out.contains(R"("closure_source":"derived")"));
}
