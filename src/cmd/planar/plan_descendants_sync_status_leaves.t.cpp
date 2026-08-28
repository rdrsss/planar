// @file plan_descendants_sync_status_leaves.t.cpp
// @brief In-process tests for the two leaves plan 996 task 6298 landed:
// `plan descendants` and `sync status`.
//
// They share a file because they share nothing else — one walks the plan
// tree, one lists external links — and neither is large enough to earn its
// own, on the same judgement `plan_task_remainder_leaves.t.cpp` made when it
// put the `plan step` family beside the `task touches` family.
//
// ## THE FIRST CASE IN THIS FILE ASSERTS THE FIXTURE, AND IT ALREADY PAID
//
// Every other case here is a byte comparison, and a fixture that silently
// seeded nothing would satisfy all of them by matching nothing. The `tree`
// cycle's file established the remedy — a dedicated first case pinning the
// fixture's own shape (depth, row counts, sibling counts) before any
// comparison runs — after a cycle shipped 13 probes with zero survivors.
//
// It is not a formality here. The oracle differential written alongside
// this file carried the same assertion and FAILED on it: the check expected
// `plan list --json` to report four plans and got zero, because the fixture
// creates its plans at `--scope global` while `plan list` from the project
// cwd is scope-filtered. The comparison cases had all "passed" on the run
// before the assertion was added. The wrong thing there was the
// EXPECTATION, not the fixture — which is exactly the case a count-only
// check cannot tell apart from a real failure.
//
// ## EVERY ABSENCE IS PAIRED WITH A PRESENCE
//
// `sync status` has an empty-result path that is easy to reach by accident:
// an unknown `--system` exits 0 with `no external links`, and under `--json`
// with ZERO BYTES. A suite that only exercised those would pass against a
// handler that returned empty unconditionally. So every filter case here
// asserts BOTH the filtered-in row set and that the excluded rows are still
// in `external_links` afterwards — a read filter that quietly became a
// delete is the failure a count alone would report as success.
//
// ## ORACLE PROVENANCE
//
// Every expected byte was captured from `zig/zig-out/bin/planar` in a pinned
// scratch arena, through a PIPE on both streams, with the exit code read
// from a file written inside the pipeline. Both rules are load-bearing and
// both are documented in `src/cmd/parity_harness.hpp`: the Zig runtime's
// positional writes make a second write to a redirected FILE land at offset
// 0 and eat the first, and a status read through a pipe reports `cat`'s.
//
// The captures were then replayed as a 32-case differential against the
// built C++ binary in a second identically-seeded arena; all 32 agreed on
// stdout, stderr and exit code. Six break-probes were then run against the
// implementation; the BFS-ordering one SURVIVED on the first fixture and is
// what forced the second grandchild into `seed_tree` (see there). The
// captures that decided a shape:
//
//   $Z plan descendants 1 --json   with three `task add --plan N` tasks and
//       NO entity_links rows -> the five PLANS and ZERO tasks. `plan next`
//       on the same database listed all three tasks. The two verbs
//       genuinely disagree about what a plan's tasks are; `descendants`
//       reads `entity_links(relationship='derives-from')` and never
//       consults `tasks.plan_id`.
//
//   $Z plan descendants 1 --json   with task:1 linked to BOTH plan:1 and
//       plan:2 -> `{"kind":"task",...,"id":1,...}` appears TWICE, adjacent.
//       The `distinct` is per-query and the query runs once per plan. A
//       defect; reproduced deliberately, and it wants its own row.
//
//   $Z plan descendants 1          -> `plan (anchor):1  Anchor` — the text
//       form fuses kind and role into one prefix where JSON keeps them as
//       two fields, so the two shapes are not mechanically derivable from
//       one another.
//
//   $Z plan descendants abc        exit 2, `error: plan id must be an
//       integer, got 'abc'` — `plan id`, with a SPACE, not `plan-id`.
//   $Z plan descendants 999        exit 1, `error: no plan with id 999`.
//
//   $Z sync status                 with a `question:1` link -> the entity
//       column MISALIGNS. The oracle's row format pads only the ID
//       (`{s}:{d:<11}`), so a kind longer than four characters pushes the
//       rest of the line right. Reproduced verbatim; "fixing" the alignment
//       would be a silent divergence.
//   $Z sync status --system nope        exit 0, `no external links\n`.
//   $Z sync status --system nope --json exit 0, ZERO BYTES — not `[]`.
//   $Z sync status --entity bogus       exit 2, before any query runs.
//   $Z sync status --system nope        exit 0 — an unknown SYSTEM is an
//       empty result while an unparseable ENTITY is an error. The asymmetry
//       is the oracle's.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

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
                         std::format("planar_6298_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run one statement against the fixture database, failing loudly.
/// @param conn The connection.
/// @param sql The statement.
void exec(planar::db::connection& conn, std::string_view sql) {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// @brief Count rows in one table.
/// @param conn The connection.
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

/// @brief Seed the plan tree the `plan descendants` cases read.
///
/// Shape, and every part of it is load-bearing:
///
///   plan 1 "Anchor"     <- the walk root
///     plan 2 "ChildA"   <- a direct child
///       plan 4 "GrandC" <- a grandchild
///     plan 3 "ChildB"   <- a direct child
///       plan 5 "GrandD" <- a SECOND grandchild, under the OTHER child
///   task 1 derives-from plan 1 AND plan 2  <- the duplicate-emission case
///   task 2 derives-from plan 2
///   task 3 attached by `--plan 4` and NEVER linked  <- the survivor that
///          proves `tasks.plan_id` is not consulted
///   task 4 linked to plan 1 with relationship `cites` <- the survivor that
///          proves the `derives-from` predicate is actually applied
///
/// THE SECOND GRANDCHILD IS NOT SYMMETRY, IT IS THE BFS PROBE. An earlier
/// version of this fixture gave only plan 2 a child, and a break-probe that
/// swapped the walk's queue for a LIFO stack SURVIVED: with plan 3
/// childless, breadth-first (2, 3, 4) and depth-first (2, 3, 4) produce the
/// same sequence, so the ordering assertion was measuring nothing. Giving
/// BOTH direct children a child of their own separates them — BFS emits
/// 2, 3, 4, 5 where a stack emits 2, 3, 5, 4 — and the probe then kills the
/// mutant. Do not simplify this tree back.
///
/// Plans are created at `--scope global` because the fixture registers no
/// association; that also means `plan list` without `--scope global` reports
/// nothing, which is what the oracle differential's fixture check caught.
/// @param fx The fixture.
void seed_tree(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--slug", "anchor", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "ChildA", "--slug", "child-a", "--parent", "1", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "ChildB", "--slug", "child-b", "--parent", "1", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "GrandC", "--slug", "grand-c", "--parent", "2", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "GrandD", "--slug", "grand-d", "--parent", "3", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T-one", "--plan", "1", "--slug", "t-one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T-two", "--plan", "2", "--slug", "t-two", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T-three", "--plan", "4", "--slug", "t-three", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "plan:1", "--relationship", "derives-from"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:2", "plan:2", "--relationship", "derives-from"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:1", "plan:2", "--relationship", "derives-from"}).code == 0);
  // A fourth task linked to the ANCHOR with a relationship that is NOT
  // `derives-from`. Without it the walk's relationship predicate is
  // untested: a break-probe that DELETED the predicate outright SURVIVED,
  // because every other edge in this fixture is already `derives-from` and
  // dropping the filter changed nothing. This row is the one the predicate
  // has to exclude, and it is what turns that probe into a kill.
  REQUIRE(dispatch(fx, {"task", "add", "T-four", "--plan", "1", "--slug", "t-four", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"links", "add", "task:4", "plan:1", "--relationship", "cites"}).code == 0);
}

/// @brief Seed two systems and four links for the `sync status` cases.
///
/// SQL rather than the CLI because the only verb that writes an
/// `external_links` row is `ext create`, which is unported and would need a
/// network. The set is chosen so that every rendering branch is REACHED:
/// two links carry a `last_synced_at` and two are NULL (the key is omitted
/// from JSON, and the text column reads `never`); all four
/// `last_sync_status` values appear; two systems and three entity kinds
/// make `--system` and `--entity` discriminate rather than match everything.
/// @param fx The fixture.
void seed_links(const fixture& fx) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  exec(*conn, "insert into external_systems (id,kind,slug,base_url,default_project,auth_method,auth_ref) values "
              "(1,'jira','jira-a','https://j.example','PROJ','token-env','JIRA_TOKEN'), "
              "(2,'github-issues','gh-b','https://api.github.com','o/r','token-env','GH_TOKEN')");
  exec(*conn, "insert into external_links (id,entity_kind,entity_id,system_id,external_id,external_url,link_role,"
              "sync_direction,last_synced_at,last_sync_status) values "
              "(1,'task',1,1,'PROJ-11','https://j.example/browse/PROJ-11','mirror','two-way','2026-01-02T03:04:05.678Z','ok'), "
              "(2,'task',2,2,'o/r#7','https://github.com/o/r/issues/7','mirror','two-way',NULL,'never'), "
              "(3,'plan',1,1,'PROJ-12',NULL,'parent','read-only','2026-02-03T04:05:06.789Z','conflict'), "
              "(4,'question',1,2,'o/r#8',NULL,'reference','write-back',NULL,'error')");
}

} // namespace

TEST_CASE("the 6298 fixture seeds the tree and the links its cases read", "[cmd][6298][fixture]") {
  // Deliberately first and deliberately about nothing else. See the header:
  // the sibling oracle differential's copy of this assertion failed on a
  // wrong EXPECTATION while every byte comparison beside it passed.
  auto const fx = make_fixture("fixture");
  seed_tree(fx);
  seed_links(fx);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "plans") == 5);
  CHECK(count(*conn, "tasks") == 4);
  CHECK(count(*conn, "external_links") == 4);
  CHECK(count(*conn, "external_systems") == 2);

  // The plan SHAPE, not just the count. A flat set of four plans and the
  // intended two-level tree have the same count and different walk output.
  auto parents = conn->prepare("select id, coalesce(parent_plan_id,0) from plans order by id");
  REQUIRE(parents.has_value());
  std::vector<std::pair<std::int64_t, std::int64_t>> got;
  while (parents->step().value() == planar::db::step_result::row) {
    got.emplace_back(parents->column_int64(0), parents->column_int64(1));
  }
  std::vector<std::pair<std::int64_t, std::int64_t>> const want{{1, 0}, {2, 1}, {3, 1}, {4, 2}, {5, 3}};
  CHECK(got == want);

  // THREE derives-from edges, and task 1 owns TWO of them. That second edge
  // is the whole duplicate-emission case; without it the relevant assertion
  // below would pass against a de-duplicating implementation.
  CHECK(count(*conn, "entity_links") == 4);
  auto dup = conn->prepare("select count(*) from entity_links where from_kind='task' and from_id=1 "
                           "and to_kind='plan' and relationship='derives-from'");
  REQUIRE(dup.has_value());
  REQUIRE(dup->step().value() == planar::db::step_result::row);
  CHECK(dup->column_int64(0) == 2);

  // Task 3 exists, is attached to plan 4 by `plan_id`, and has NO link. It
  // is the survivor that makes "plan_id is not consulted" falsifiable.
  auto orphan = conn->prepare("select t.plan_id, (select count(*) from entity_links el where el.from_kind='task' "
                              "and el.from_id=t.id) from tasks t where t.id=3");
  REQUIRE(orphan.has_value());
  REQUIRE(orphan->step().value() == planar::db::step_result::row);
  CHECK(orphan->column_int64(0) == 4);
  CHECK(orphan->column_int64(1) == 0);

  // Exactly two of the four links carry a timestamp, so the JSON
  // key-omission branch and the text `never` branch are both reachable.
  auto stamped = conn->prepare("select count(*) from external_links where last_synced_at is not null");
  REQUIRE(stamped.has_value());
  REQUIRE(stamped->step().value() == planar::db::step_result::row);
  CHECK(stamped->column_int64(0) == 2);
}

TEST_CASE("plan descendants walks anchor then children breadth-first, then tasks", "[cmd][6298][plan-descendants]") {
  auto const fx = make_fixture("walk");
  seed_tree(fx);

  auto const json = dispatch(fx, {"plan", "descendants", "1", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  CHECK(json.out == R"([{"kind":"plan","role":"anchor","id":1,"title":"Anchor"},)"
                    R"({"kind":"plan","role":"child","id":2,"title":"ChildA"},)"
                    R"({"kind":"plan","role":"child","id":3,"title":"ChildB"},)"
                    R"({"kind":"plan","role":"child","id":4,"title":"GrandC"},)"
                    R"({"kind":"plan","role":"child","id":5,"title":"GrandD"},)"
                    R"({"kind":"task","role":"task","id":1,"title":"T-one"},)"
                    R"({"kind":"task","role":"task","id":1,"title":"T-one"},)"
                    R"({"kind":"task","role":"task","id":2,"title":"T-two"}])"
                    "\n");

  // BFS, not DFS. Plan 4's parent is plan 2, so a depth-first walk would
  // emit 2, 4, 3. Both orders contain the same ids, so only the SEQUENCE
  // discriminates and a set comparison here would prove nothing.
  auto const pos2 = json.out.find(R"("id":2)");
  auto const pos3 = json.out.find(R"("id":3)");
  auto const pos4 = json.out.find(R"("id":4)");
  REQUIRE(pos2 != std::string::npos);
  REQUIRE(pos3 != std::string::npos);
  REQUIRE(pos4 != std::string::npos);
  INFO("grandchild 4 must follow BOTH direct children");
  CHECK(pos2 < pos3);
  CHECK(pos3 < pos4);

  // The text form fuses kind and role into one prefix. It is not derivable
  // from the JSON by string substitution, so it gets its own expectation.
  auto const text = dispatch(fx, {"plan", "descendants", "1"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.out == "plan (anchor):1  Anchor\n"
                    "plan (child):2  ChildA\n"
                    "plan (child):3  ChildB\n"
                    "plan (child):4  GrandC\n"
                    "plan (child):5  GrandD\n"
                    "task:1  T-one\n"
                    "task:1  T-one\n"
                    "task:2  T-two\n");
}

TEST_CASE("plan descendants ignores tasks.plan_id and reads derives-from only", "[cmd][6298][plan-descendants]") {
  auto const fx = make_fixture("planid");
  seed_tree(fx);

  // Task 3 is attached to plan 4 by `--plan` and is NOT linked. Plan 4 is
  // inside the walked tree, so a walk that consulted `plan_id` would list
  // it. The oracle does not.
  auto const json = dispatch(fx, {"plan", "descendants", "1", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.find(R"("id":3,"title":"T-three")") == std::string::npos);
  CHECK(json.out.find("T-three") == std::string::npos);

  // The PRESENT half of the same claim: the two tasks that ARE linked do
  // appear. Without this, the absence above would also pass against a walk
  // that emitted no tasks at all.
  CHECK(json.out.find("T-one") != std::string::npos);
  CHECK(json.out.find("T-two") != std::string::npos);

  // Task 4 IS linked to the anchor, but with `cites` rather than
  // `derives-from`, so it is excluded too. This is the other half of the
  // predicate: a walk that dropped the relationship filter would emit it.
  CHECK(json.out.find("T-four") == std::string::npos);

  // And both excluded tasks are still in the table — a read verb must not
  // have removed them.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "tasks") == 4);
  CHECK(count(*conn, "entity_links") == 4);
}

TEST_CASE("plan descendants emits a multiply-linked task once per plan", "[cmd][6298][plan-descendants]") {
  auto const fx = make_fixture("dup");
  seed_tree(fx);

  // Task 1 derives-from plan 1 and plan 2, both inside the tree, so it is
  // emitted TWICE. This is an oracle defect reproduced deliberately; see
  // the module header and the task report.
  auto const json = dispatch(fx, {"plan", "descendants", "1", "--json"});
  REQUIRE(json.code == 0);
  std::size_t occurrences = 0;
  for (std::size_t at = json.out.find(R"("title":"T-one")"); at != std::string::npos;
       at             = json.out.find(R"("title":"T-one")", at + 1)) {
    ++occurrences;
  }
  CHECK(occurrences == 2);

  // Walking from plan 2 instead reaches task 1 through ONE plan only, so
  // the same task appears once. That contrast is what shows the repetition
  // is per-plan rather than an unconditional double-emit.
  auto const from_child = dispatch(fx, {"plan", "descendants", "2", "--json"});
  REQUIRE(from_child.code == 0);
  std::size_t once = 0;
  for (std::size_t at = from_child.out.find(R"("title":"T-one")"); at != std::string::npos;
       at             = from_child.out.find(R"("title":"T-one")", at + 1)) {
    ++once;
  }
  CHECK(once == 1);
}

TEST_CASE("plan descendants renders a childless anchor and refuses bad input", "[cmd][6298][plan-descendants]") {
  auto const fx = make_fixture("edges");
  seed_tree(fx);

  // Plan 4 is a leaf with no children and no linked tasks. The anchor's OWN
  // entry is still emitted — the result is never empty for an existing plan.
  auto const lone = dispatch(fx, {"plan", "descendants", "4", "--json"});
  CHECK(lone.code == 0);
  CHECK(lone.out == R"([{"kind":"plan","role":"anchor","id":4,"title":"GrandC"}])"
                    "\n");
  // The prefix is `plan (anchor)`, not `plan (child)`: the role is relative
  // to the WALK, so a plan that is someone's child becomes the anchor when
  // walked from. Measured, and worth measuring — the first draft of this
  // case asserted `plan (child)` on the reasoning that plan 4's row carries
  // a `parent_plan_id`, and the oracle disagreed. The text and JSON forms
  // agree with each other here; there is no inconsistency to report.
  auto const lone_text = dispatch(fx, {"plan", "descendants", "4"});
  CHECK(lone_text.code == 0);
  CHECK(lone_text.out == "plan (anchor):4  GrandC\n");

  // A plan with exactly one descendant, to sit between the eight-row anchor
  // walk and the one-row leaf above. Plan 3 is a child that has a child.
  auto const mid = dispatch(fx, {"plan", "descendants", "3", "--json"});
  CHECK(mid.code == 0);
  CHECK(mid.out == R"([{"kind":"plan","role":"anchor","id":3,"title":"ChildB"},)"
                   R"({"kind":"plan","role":"child","id":5,"title":"GrandD"}])"
                   "\n");
  auto const mid_text = dispatch(fx, {"plan", "descendants", "3"});
  CHECK(mid_text.code == 0);
  CHECK(mid_text.out == "plan (anchor):3  ChildB\n"
                        "plan (child):5  GrandD\n");

  auto const missing = dispatch(fx, {"plan", "descendants", "999", "--json"});
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  CHECK(missing.err == "error: no plan with id 999\n");

  // `plan id`, with a SPACE. `plan-id` is the positional's declared name,
  // not the noun the message uses.
  auto const bad = dispatch(fx, {"plan", "descendants", "abc"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: plan id must be an integer, got 'abc'\n");
}

TEST_CASE("sync status lists every link when unfiltered", "[cmd][6298][sync-status]") {
  auto const fx = make_fixture("list");
  seed_tree(fx);
  seed_links(fx);

  auto const text = dispatch(fx, {"sync", "status"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // The `question:1` row MISALIGNS, and that is the oracle's. Only the id
  // carries the column width, so a five-character kind pushes the rest of
  // the line right. Straightening it here would be a silent divergence.
  CHECK(text.out == "link    entity          external-id         system    last-sync                 status\n"
                    "1       task:1            PROJ-11             1         2026-01-02T03:04:05.678Z  ok\n"
                    "2       task:2            o/r#7               2         never                     never\n"
                    "3       plan:1            PROJ-12             1         2026-02-03T04:05:06.789Z  conflict\n"
                    "4       question:1            o/r#8               2         never                     error\n");

  // Line-delimited objects, NOT an array. And `last_synced_at` is OMITTED
  // when NULL rather than rendered as `null`, so the key set varies row to
  // row — links 2 and 4 carry six keys where 1 and 3 carry seven.
  auto const json = dispatch(fx, {"sync", "status", "--json"});
  CHECK(json.code == 0);
  CHECK(
      json.out ==
      R"({"link_id":1,"entity_kind":"task","entity_id":1,"external_id":"PROJ-11","system_id":1,"last_synced_at":"2026-01-02T03:04:05.678Z","last_sync_status":"ok"})"
      "\n"
      R"({"link_id":2,"entity_kind":"task","entity_id":2,"external_id":"o/r#7","system_id":2,"last_sync_status":"never"})"
      "\n"
      R"({"link_id":3,"entity_kind":"plan","entity_id":1,"external_id":"PROJ-12","system_id":1,"last_synced_at":"2026-02-03T04:05:06.789Z","last_sync_status":"conflict"})"
      "\n"
      R"({"link_id":4,"entity_kind":"question","entity_id":1,"external_id":"o/r#8","system_id":2,"last_sync_status":"error"})"
      "\n");
  CHECK(json.out.find("null") == std::string::npos);
  CHECK(json.out.front() != '[');
}

TEST_CASE("sync status filters by system and by entity, and excludes by a survivor", "[cmd][6298][sync-status]") {
  auto const fx = make_fixture("filter");
  seed_tree(fx);
  seed_links(fx);

  // --system keeps 1 and 3 and drops 2 and 4.
  auto const jira = dispatch(fx, {"sync", "status", "--system", "jira-a", "--json"});
  CHECK(jira.code == 0);
  CHECK(jira.out.find(R"("link_id":1)") != std::string::npos);
  CHECK(jira.out.find(R"("link_id":3)") != std::string::npos);
  CHECK(jira.out.find(R"("link_id":2)") == std::string::npos);
  CHECK(jira.out.find(R"("link_id":4)") == std::string::npos);

  // The complementary system returns the OTHER two. An inert filter would
  // return all four here and to the query above; a uniformly-broken one
  // would return none to both. Only a working filter splits them.
  auto const gh = dispatch(fx, {"sync", "status", "--system", "gh-b", "--json"});
  CHECK(gh.code == 0);
  CHECK(gh.out.find(R"("link_id":2)") != std::string::npos);
  CHECK(gh.out.find(R"("link_id":4)") != std::string::npos);
  CHECK(gh.out.find(R"("link_id":1)") == std::string::npos);
  CHECK(gh.out.find(R"("link_id":3)") == std::string::npos);

  // --entity discriminates on BOTH halves of the ref. `task:1` and `plan:1`
  // share an id and differ only by kind; `task:1` and `task:2` share a kind
  // and differ only by id. One case each would leave half the predicate
  // untested.
  auto const task1 = dispatch(fx, {"sync", "status", "--entity", "task:1", "--json"});
  CHECK(task1.code == 0);
  CHECK(task1.out.find(R"("link_id":1)") != std::string::npos);
  CHECK(task1.out.find(R"("link_id":3)") == std::string::npos);

  auto const plan1 = dispatch(fx, {"sync", "status", "--entity", "plan:1", "--json"});
  CHECK(plan1.code == 0);
  CHECK(plan1.out.find(R"("link_id":3)") != std::string::npos);
  CHECK(plan1.out.find(R"("link_id":1)") == std::string::npos);

  auto const task2 = dispatch(fx, {"sync", "status", "--entity", "task:2", "--json"});
  CHECK(task2.code == 0);
  CHECK(task2.out.find(R"("link_id":2)") != std::string::npos);
  CHECK(task2.out.find(R"("link_id":1)") == std::string::npos);

  // The two filters COMPOSE rather than one overriding the other: task:1 is
  // on jira-a, so pairing it with gh-b must return nothing even though each
  // half alone matches something.
  auto const both_hit = dispatch(fx, {"sync", "status", "--entity", "task:1", "--system", "jira-a", "--json"});
  CHECK(both_hit.code == 0);
  CHECK(both_hit.out.find(R"("link_id":1)") != std::string::npos);
  auto const both_miss = dispatch(fx, {"sync", "status", "--entity", "task:1", "--system", "gh-b", "--json"});
  CHECK(both_miss.code == 0);
  CHECK(both_miss.out.empty());

  // THE SURVIVOR CHECK: every excluded row is still in the table. A read
  // filter that had become a blast radius would satisfy every assertion
  // above and fail this one.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "external_links") == 4);
  CHECK(count(*conn, "external_systems") == 2);
}

TEST_CASE("sync status treats an unknown system as empty and a bad entity as an error", "[cmd][6298][sync-status]") {
  auto const fx = make_fixture("empty");
  seed_tree(fx);
  seed_links(fx);

  // An unknown SYSTEM is a normal empty result. Note this runs against a
  // fixture with four links, so "empty" here is the filter's doing and not
  // an empty database — the same assertion on a bare fixture would pass
  // against a handler that never queried anything.
  auto const unknown_text = dispatch(fx, {"sync", "status", "--system", "nope"});
  CHECK(unknown_text.code == 0);
  CHECK(unknown_text.err.empty());
  CHECK(unknown_text.out == "no external links\n");

  // Under --json the same case emits ZERO BYTES. Not `[]`, which is what a
  // reasonable-looking port would produce and what a JSON-parsing assertion
  // would fail to distinguish.
  auto const unknown_json = dispatch(fx, {"sync", "status", "--system", "nope", "--json"});
  CHECK(unknown_json.code == 0);
  CHECK(unknown_json.out.empty());
  CHECK(unknown_json.err.empty());

  // An unparseable ENTITY refuses at exit 2 instead. The asymmetry with
  // `--system` above is the oracle's and is the point of pairing them here.
  for (auto const& bad : {"bogus", "task:xyz", "", ":task:1"}) {
    INFO("malformed --entity: '" << bad << "'");
    auto const refused = dispatch(fx, {"sync", "status", "--entity", bad});
    CHECK(refused.code == 2);
    CHECK(refused.out.empty());
    CHECK(refused.err == std::format("error: invalid --entity value '{}'; expected <kind>:<integer-id>\n", bad));
  }

  // A well-formed ref naming a kind that exists but a row that does not is
  // NOT an error — it is an empty result, like the unknown system.
  auto const no_such_row = dispatch(fx, {"sync", "status", "--entity", "task:999", "--json"});
  CHECK(no_such_row.code == 0);
  CHECK(no_such_row.out.empty());

  // An empty --system, unlike an empty --entity, is accepted and matches
  // nothing. Two empty strings, two different meanings, one verb.
  auto const empty_system = dispatch(fx, {"sync", "status", "--system", ""});
  CHECK(empty_system.code == 0);
  CHECK(empty_system.out == "no external links\n");
}
