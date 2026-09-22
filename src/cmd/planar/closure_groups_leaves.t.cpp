// @file closure_groups_leaves.t.cpp
// @brief In-process tests for the `closure` and `groups` leaves wired by plan
// 996, task 6189.
//
// ## THESE TWO LEAVES WRITE NOTHING, SO THE ROW ASSERTIONS RUN THE OTHER WAY
//
// Everywhere else in this tree a row assertion checks what a verb WROTE.
// Both leaves here are read-only, so the rows are the INPUT and the
// assertion is that a change to a row changes the answer — which is the only
// way to prove a query is actually consulted rather than approximated. Every
// case therefore seeds `closures` / `tasks` / `entity_links` through direct
// SQL, asserts the rendered answer, and — for each of the three filters —
// asserts that the row a filter must EXCLUDE is still present in the table
// while being absent from the output.
//
// A `select count(*)` on the untouched table backs each of those, so
// "excluded" can never be satisfied by "the seed did not land".
//
// ## THE THREE FILTERS, EACH PROVEN TO EXCLUDE WITH A SURVIVOR NAMED
//
// `groups recommend` applies three independent restrictions, and an inert
// version of any one of them returns a plausible answer at exit 0:
//
//   1. **status = 'todo'.** A `done` task with a closure must contribute
//      neither an id nor a symbol nor a cost. Proved with a done task whose
//      symbol weight (5000) would dominate the totals if it leaked.
//   2. **role != 'transitive'.** A `transitive` row is excluded from the
//      effective closure. Proved with weight 9000 on the transitive row: a
//      slice that included it could not possibly report the asserted cost.
//   3. **plan_id.** A task in a DIFFERENT plan must not appear. Proved by
//      asking for BOTH plans in the same fixture — the foreign task is
//      absent from plan 1's answer and PRESENT in plan 2's, so the filter
//      is discriminating rather than simply dropping everything.
//
// Every one of the three uses a distinctive weight, so the assertion is on
// the reported cost and the named symbol, never on a count.
//
// A fourth restriction is asserted the same way on `load_deps`: an edge is
// kept only when BOTH endpoints are open tasks of the plan. The fixture
// carries an edge to a `done` task and a self-edge, and the grouping the
// engine produces is asserted by value.
//
// ## `closure show` CANNOT FAIL INTO A NOT-FOUND
//
// `closure show 999` on a database with no task 999 is exit 0 and
// `{"task_id":999,"rows":[]}` — the id is ECHOED from the argument, not read
// back from a row. Pinned, because the obvious "improvement" would break
// every caller that polls a closure before computing it. Contrast
// `groups recommend 999`, which IS a not-found at exit 1; the two leaves
// genuinely disagree and both spellings are here.
//
// ## ORDERING IS ASSERTED WHERE A TIE CAN DISCRIMINATE
//
// `closure show` orders by role (an explicit CASE: modify, reference, then
// everything else), then path, then symbol. Role and path-vs-symbol are
// separable only with a constructed tie, so the fixture seeds rows whose
// PATH order and SYMBOL order disagree (`a/first.zig::z.zzz` versus
// `z/last.zig::a.aaa`) and asserts path wins. Insertion id is not consulted
// at all, which the seed order proves by inserting them backwards.
//
// A KNOWN, DOCUMENTED LIMIT of that assertion, recorded here rather than
// papered over: replacing the role CASE with a plain `order by role`
// SURVIVES this test, and survives `closure.store`'s own ordering case for
// the same reason. `modify` < `reference` < `transitive` alphabetically, so
// the two orderings agree on every value the schema's CHECK constraint
// permits. Discriminating them would require a row the constraint forbids,
// which would be a test of SQLite rather than of this contract. The CASE's
// `else 2` arm is deliberate future-proofing (see store.cppm), not a
// behaviour any legal fixture can observe.
//
// ## HOME / DB SAFETY
//
// Every fixture builds an explicit environment map over its own scratch root
// and an explicit scratch `db_path`; nothing here reads the process
// environment, so the operator's `~/.planar/planar.db` is unreachable.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;
import planar.engine.grouping.mtkahypar;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment and database path.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_clogrp_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Open the fixture's database directly, for seeding and row reads.
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

/// @brief Create the database and one association-scoped plan with tasks.
///
/// Seeded through the CLI so the same code paths an operator would take
/// establish the scope. The read-only renderer cases extend it with direct
/// SQL; the dedicated compute case exercises the real writer end-to-end.
/// @param fx The fixture.
auto seed_plan(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--json", "--allow-no-repo"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "feat", "--name", "feat", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "feat", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Demo", "--slug", "demo", "--summary", "S", "--json"}).code == 0);
}

/// @brief Insert one `closures` row.
/// @param conn An open connection.
/// @param task_id Owning task.
/// @param path Repo-relative path.
/// @param symbol Qualified symbol.
/// @param role `modify` / `reference` / `transitive`.
/// @param weight Token weight.
/// @param version Extractor version.
auto add_closure(planar::db::connection& conn, int task_id, std::string_view path, std::string_view symbol, std::string_view role,
                 int weight, std::string_view version = "m2-closure-0.1") -> void {
  exec(conn, std::format(R"(insert into closures
                             (task_id, repo_id, path, symbol, role, token_weight, extractor_version, created_at)
                           values ({}, 1, '{}', '{}', '{}', {}, '{}', '2026-01-01T00:00:00.000Z'))",
                         task_id, path, symbol, role, weight, version));
}

} // namespace

TEST_CASE("closure compute is wired end-to-end and preserves its Zig JSON contract", "[cmd][closure][compute]") {
  auto const fx = make_fixture("clocompute");
  seed_plan(fx);
  REQUIRE(dispatch(fx, {"task", "add", "T1", "--plan", "1", "--editor=false", "--json"}).code == 0);
  std::ofstream{fx.root / "proj" / "seed.zig"} << "fn run() void {}\n";
  {
    auto conn = open_db(fx);
    exec(conn, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  }
  auto const computed = dispatch(fx, {"closure", "compute", "1", "--json"});
  CHECK(computed.code == 0);
  CHECK(computed.err.empty());
  CHECK(computed.out ==
        R"({"task_id":1,"seeds":1,"modify":1,"reference":0,"transitive":0,"rows_written":1,"extractor_version":"m2-closure-0.1"})"
        "\n");
  {
    auto conn = open_db(fx);
    CHECK(query(conn, "select repo_id,path,symbol,role,token_weight from closures", 5) == "1|seed.zig|seed.run|modify|7");
  }

  auto const no_seeds = dispatch(fx, {"task", "add", "T2", "--plan", "1", "--editor=false", "--json"});
  REQUIRE(no_seeds.code == 0);
  auto const refusal = dispatch(fx, {"closure", "compute", "2", "--json"});
  CHECK(refusal.code == 2);
  CHECK(refusal.out == planar::cmd::testsupport::json_error_envelope_line("closure compute", "invalid_input"));
  CHECK(refusal.err == "error: closure compute: task 2 declares no path-level touches (task_touch_paths); nothing to compute\n");
}

TEST_CASE("closure compute admits an association-to-member write", "[cmd][closure][compute][6735]") {
  // Decision 1121 (task 6735): `closure compute` was one of only two guarded
  // verbs still comparing scopes by STRICT EQUALITY, so an `assoc:<org>`
  // operator was refused on an entity stored at `repo:<member>` even though
  // that repo belongs to that association -- while six sibling verbs
  // accepted the identical write. `seed_plan` already registers `feat` with
  // `proj` as a member, which is the relation this exercises.
  auto const fx = make_fixture("clomembership");
  seed_plan(fx);
  REQUIRE(dispatch(fx, {"task", "add", "T1", "--plan", "1", "--scope", "repo:proj", "--editor=false", "--json"}).code == 0);
  std::ofstream{fx.root / "proj" / "seed.zig"} << "fn run() void {}\n";
  {
    auto conn = open_db(fx);
    exec(conn, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  }

  auto const computed = dispatch(fx, {"closure", "compute", "1", "--scope", "feat", "--json"});
  INFO("stderr: " << computed.err);
  CHECK(computed.code == 0);
  {
    auto conn = open_db(fx);
    CHECK(query(conn, "select count(*) from closures", 1) == "1");
  }
}

TEST_CASE("closure show orders by role, then PATH, then symbol", "[cmd][closure][show]") {
  auto const fx = make_fixture("closorder");
  seed_plan(fx);
  REQUIRE(dispatch(fx, {"task", "add", "T1", "--plan", "1", "--editor=false", "--json"}).code == 0);

  {
    auto conn = open_db(fx);
    // Inserted in an order that DISAGREES with every asserted ordering key,
    // so a query that fell back to insertion id could not produce the
    // expected answer. The two `modify` rows are the constructed tie: path
    // order (`a/…` before `z/…`) and symbol order (`a.aaa` before `z.zzz`)
    // point in opposite directions.
    add_closure(conn, 1, "m/mid.zig", "m.mmm", "reference", 5);
    add_closure(conn, 1, "z/last.zig", "a.aaa", "modify", 10);
    add_closure(conn, 1, "a/first.zig", "z.zzz", "modify", 20);
    add_closure(conn, 1, "t/tr.zig", "t.ttt", "transitive", 9000);
  }

  auto const text = dispatch(fx, {"closure", "show", "1"});
  CHECK(text.code == 0);
  // modify rows first, PATH-ordered within the role; then reference; then
  // transitive, which `show` does NOT exclude (only `groups recommend`
  // does — the two leaves read the same table differently).
  CHECK(text.out == "closure for task 1 (4 rows):\n"
                    "  [modify] a/first.zig::z.zzz  w=20\n"
                    "  [modify] z/last.zig::a.aaa  w=10\n"
                    "  [reference] m/mid.zig::m.mmm  w=5\n"
                    "  [transitive] t/tr.zig::t.ttt  w=9000\n");

  auto conn = open_db(fx);
  // The rows really are there and really are in the opposite insertion
  // order, so the ordering above is the QUERY's doing.
  CHECK(query(conn, "select cast(id as text) || '|' || path from closures order by id", 1) ==
        "1|m/mid.zig\n2|z/last.zig\n3|a/first.zig\n4|t/tr.zig");
}

TEST_CASE("closure show returns rows from EVERY extractor version, undistinguished", "[cmd][closure][show]") {
  auto const fx = make_fixture("closver");
  seed_plan(fx);
  REQUIRE(dispatch(fx, {"task", "add", "T1", "--plan", "1", "--editor=false", "--json"}).code == 0);
  {
    auto conn = open_db(fx);
    add_closure(conn, 1, "a.zig", "a.aaa", "modify", 10, "m2-closure-0.1");
    add_closure(conn, 1, "a.zig", "a.aaa", "modify", 77, "m2-closure-0.2");
  }

  // The UNIQUE key includes `extractor_version`, so two versions of the same
  // symbol coexist — and `show` does not filter by version even though it
  // could. BOTH rows come back, which is what makes a stale-extractor
  // closure visible to an operator instead of silently shadowed.
  auto const json = dispatch(fx, {"closure", "show", "1", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.contains("\"token_weight\":10,\"extractor_version\":\"m2-closure-0.1\""));
  CHECK(json.out.contains("\"token_weight\":77,\"extractor_version\":\"m2-closure-0.2\""));
}

TEST_CASE("closure show on an unknown task is exit 0 with an ECHOED id", "[cmd][closure][show]") {
  auto const fx = make_fixture("closmiss");
  seed_plan(fx);

  // NOT a not-found. The envelope names a task that does not exist, because
  // the id comes from the argument rather than from a row.
  auto const json = dispatch(fx, {"closure", "show", "999", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  CHECK(json.out == "{\"task_id\":999,\"rows\":[]}\n");

  auto const text = dispatch(fx, {"closure", "show", "999"});
  CHECK(text.code == 0);
  // The empty case is a parenthesised hint naming the exact command, with an
  // em dash — not an empty list.
  CHECK(text.out == "closure for task 999 (0 rows):\n"
                    "  (none \xe2\x80\x94 run `planar closure compute 999` first)\n");

  auto conn = open_db(fx);
  CHECK(query(conn, "select cast(count(*) as text) from tasks where id = 999", 1) == "0");
}

TEST_CASE("closure show refuses a non-integer positional at exit 2", "[cmd][closure][show]") {
  auto const fx = make_fixture("closbad");
  seed_plan(fx);

  auto const bad = dispatch(fx, {"closure", "show", "notanint"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  // NOT prefixed with the leaf name, unlike almost every other diagnostic in
  // this port. Oracle-captured.
  CHECK(bad.err == "error: task id must be an integer, got 'notanint'\n");

  // The Zig integer grammar, end to end: `_` is a separator, a leading `+`
  // and leading zeros are accepted, and an overflow is a refusal naming the
  // literal the operator typed.
  CHECK(dispatch(fx, {"closure", "show", "1_0"}).out.starts_with("closure for task 10 "));
  CHECK(dispatch(fx, {"closure", "show", "007"}).out.starts_with("closure for task 7 "));
  CHECK(dispatch(fx, {"closure", "show", "+12"}).out.starts_with("closure for task 12 "));
  auto const overflow = dispatch(fx, {"closure", "show", "9223372036854775808"});
  CHECK(overflow.code == 2);
  CHECK(overflow.err == "error: task id must be an integer, got '9223372036854775808'\n");
  // Discrimination: the largest representable value is NOT a refusal, so the
  // check is a range check and not a length check.
  CHECK(dispatch(fx, {"closure", "show", "9223372036854775807"}).code == 0);
}

TEST_CASE("groups recommend EXCLUDES done tasks, transitive rows and foreign plans", "[cmd][groups][recommend][filter]") {
  auto const fx = make_fixture("grpfilter");
  seed_plan(fx);
  for (auto const title : {"T1", "T2", "T3", "T4"}) {
    REQUIRE(dispatch(fx, {"task", "add", title, "--plan", "1", "--editor=false", "--json"}).code == 0);
  }
  REQUIRE(dispatch(fx, {"plan", "create", "Other", "--slug", "other", "--summary", "S", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Foreign", "--plan", "2", "--editor=false", "--json"}).code == 0);

  {
    auto conn = open_db(fx);
    add_closure(conn, 1, "a/first.zig", "z.zzz", "modify", 20);
    // (2) TRANSITIVE, and heavy enough that a slice including it could not
    //     report the asserted cost.
    add_closure(conn, 1, "t/tr.zig", "t.ttt", "transitive", 9000);
    add_closure(conn, 2, "b/two.zig", "b.bbb", "modify", 30);
    add_closure(conn, 3, "c/three.zig", "c.ccc", "modify", 40);
    // (1) DONE, and heavy for the same reason.
    add_closure(conn, 4, "d/four.zig", "d.ddd", "modify", 5000);
    exec(conn, "update tasks set status = 'done' where id = 4");
    // (3) FOREIGN PLAN, heavy for the same reason.
    add_closure(conn, 5, "f/for.zig", "f.fff", "modify", 7000);
    // An edge from an OPEN task to a DONE one, and a self-edge. Both must be
    // dropped by `load_deps`; an engine that kept either would hand
    // `greedy::group` a DAG referring to a task outside the set.
    exec(conn, R"(insert into entity_links (from_kind, from_id, to_kind, to_id, relationship, created_at)
                  values ('task', 3, 'task', 4, 'depends-on', '2026-01-01T00:00:00.000Z'))");
    exec(conn, R"(insert into entity_links (from_kind, from_id, to_kind, to_id, relationship, created_at)
                  values ('task', 1, 'task', 1, 'depends-on', '2026-01-01T00:00:00.000Z'))");
  }

  auto const plan1 = dispatch(fx, {"groups", "recommend", "1", "--json"});
  REQUIRE(plan1.code == 0);
  // THE SURVIVORS, named. All three open tasks and all three of their
  // `modify` symbols are present, so the absences below mean something.
  CHECK(plan1.out.contains("\"open_tasks\":3"));
  CHECK(plan1.out.contains("\"z.zzz\""));
  CHECK(plan1.out.contains("\"b.bbb\""));
  CHECK(plan1.out.contains("\"c.ccc\""));
  // THE EXCLUSIONS, each by its distinctive symbol AND its distinctive
  // weight — a cost assertion is what a count could never make.
  CHECK_FALSE(plan1.out.contains("t.ttt"));
  CHECK_FALSE(plan1.out.contains("9000"));
  CHECK_FALSE(plan1.out.contains("d.ddd"));
  CHECK_FALSE(plan1.out.contains("5000"));
  CHECK_FALSE(plan1.out.contains("f.fff"));
  CHECK_FALSE(plan1.out.contains("7000"));
  CHECK(plan1.out.contains("\"total_cost\":90"));

  // THE DISCRIMINATION for the plan filter: the foreign task is not merely
  // dropped everywhere, it is REACHABLE through its own plan. A filter that
  // returned nothing for every plan would pass the absence above.
  auto const plan2 = dispatch(fx, {"groups", "recommend", "2", "--json"});
  REQUIRE(plan2.code == 0);
  CHECK(plan2.out.contains("\"open_tasks\":1"));
  CHECK(plan2.out.contains("\"f.fff\""));
  CHECK(plan2.out.contains("\"total_cost\":7000"));
  CHECK_FALSE(plan2.out.contains("z.zzz"));
  CHECK(plan1.out != plan2.out);

  // AND THE ROWS THE FILTERS EXCLUDED ARE STILL IN THE TABLE. Without this,
  // every `CHECK_FALSE` above is also satisfied by a seed that never landed.
  auto conn = open_db(fx);
  CHECK(query(conn, "select symbol || '|' || cast(token_weight as text) from closures order by token_weight", 1) ==
        "z.zzz|20\nb.bbb|30\nc.ccc|40\nd.ddd|5000\nf.fff|7000\nt.ttt|9000");
  CHECK(query(conn, "select status from tasks where id = 4", 1) == "done");
  CHECK(query(conn, "select cast(count(*) as text) from entity_links where relationship = 'depends-on'", 1) == "2");
}

TEST_CASE("groups recommend folds a symbol carried under two roles into ONE unit", "[cmd][groups][recommend]") {
  auto const fx = make_fixture("grpfold");
  seed_plan(fx);
  REQUIRE(dispatch(fx, {"task", "add", "T1", "--plan", "1", "--editor=false", "--json"}).code == 0);
  {
    auto conn = open_db(fx);
    // The SAME symbol under both roles, with different weights. It must fold
    // to a single unit taking the LARGER weight — not appear twice, and not
    // take the smaller. Both wrong answers are distinguishable by cost:
    // duplicated would be 30, smaller-wins would be 10.
    add_closure(conn, 1, "a.zig", "shared", "modify", 10);
    add_closure(conn, 1, "a.zig", "shared", "reference", 20);
  }

  auto const got = dispatch(fx, {"groups", "recommend", "1", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.out.contains("\"union_symbols\":[\"shared\"]"));
  CHECK(got.out.contains("\"cost\":20"));
  CHECK(got.out.contains("\"total_cost\":20"));
}

TEST_CASE("groups recommend reports a plan with no open tasks as a SUCCESS", "[cmd][groups][recommend]") {
  auto const fx = make_fixture("grpempty");
  seed_plan(fx);

  // A plan that EXISTS with nothing to group is zero slices at exit 0, never
  // a not-found. The two are one character apart in the SQL and produce
  // opposite exit codes, so both are pinned in the same file.
  auto const empty = dispatch(fx, {"groups", "recommend", "1"});
  CHECK(empty.code == 0);
  CHECK(empty.err.empty());
  CHECK(empty.out == "plan:1  budget:128000  open:0  solver:greedy  optimal_available:false  selected_greedy:false  "
                     "slices:0  total_cost:0\n"
                     "  (no open tasks to group)\n");

  auto const missing = dispatch(fx, {"groups", "recommend", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  // NOT quoted, unlike `test-spec status`'s `plan '<arg>' not found`.
  CHECK(missing.err == "error: plan 999 not found\n");
}

TEST_CASE("groups recommend refuses bad plan ids, budgets and solvers, each distinctly", "[cmd][groups][recommend]") {
  auto const fx = make_fixture("grpbad");
  seed_plan(fx);

  auto const bad_plan = dispatch(fx, {"groups", "recommend", "abc"});
  CHECK(bad_plan.code == 2);
  CHECK(bad_plan.err == "error: plan id must be an integer, got 'abc'\n");

  auto const bad_budget = dispatch(fx, {"groups", "recommend", "1", "--budget", "xyz"});
  CHECK(bad_budget.code == 2);
  CHECK(bad_budget.err == "error: --budget must be a non-negative integer, got 'xyz'\n");

  // The RANGE half. `--budget` is unsigned 32-bit: one past the ceiling is a
  // refusal, the ceiling itself is not. A port that parsed it as a 64-bit
  // value would accept the first, silently truncate, and report a budget the
  // operator never asked for.
  auto const overflow = dispatch(fx, {"groups", "recommend", "1", "--budget", "4294967296"});
  CHECK(overflow.code == 2);
  CHECK(overflow.err == "error: --budget must be a non-negative integer, got '4294967296'\n");
  auto const ceiling = dispatch(fx, {"groups", "recommend", "1", "--budget", "4294967295"});
  CHECK(ceiling.code == 0);
  CHECK(ceiling.out.contains("budget:4294967295"));
  // ...and a negative value is a refusal rather than a wrap to a huge one.
  auto const negative = dispatch(fx, {"groups", "recommend", "1", "--budget", "-1"});
  CHECK(negative.code == 2);
  CHECK(negative.err == "error: --budget must be a non-negative integer, got '-1'\n");
  // Zero is legal and reaches the engine, which is why the refusal wording
  // says "non-negative" rather than "positive".
  auto const zero = dispatch(fx, {"groups", "recommend", "1", "--budget", "0"});
  CHECK(zero.code == 0);
  CHECK(zero.out.contains("budget:0"));

  auto const bad_solver = dispatch(fx, {"groups", "recommend", "1", "--solver", "bogus"});
  CHECK(bad_solver.code == 2);
  // NO leaf prefix on this one — captured as a bare line.
  CHECK(bad_solver.err == "error: --solver must be 'greedy' or 'mtkahypar', got 'bogus'\n");
}

TEST_CASE("groups recommend accepts --solver mtkahypar and reports that the optimal arm did not run",
          "[cmd][groups][recommend]") {
  auto const fx = make_fixture("grpsolver");
  seed_plan(fx);
  REQUIRE(dispatch(fx, {"task", "add", "T1", "--plan", "1", "--editor=false", "--json"}).code == 0);
  {
    auto conn = open_db(fx);
    add_closure(conn, 1, "a.zig", "a.aaa", "modify", 10);
  }

  // Accepting the flag is NOT the inert-filter defect, because the outcome is
  // REPORTED either way: `solver` names the partitioner that actually ran and
  // `optimal_available` says whether the optimal one could.
  //
  // WHICH answer is correct depends on the CONFIGURE, so this case branches on
  // the same runtime probe the engine itself branches on rather than pinning
  // one arm (task 6543). Before this, the case asserted the degraded arm
  // unconditionally — correct while the solver was unported, and silently
  // wrong once decision 1032 had the parity lane build with
  // `PLANAR_WITH_MTKAHYPAR=ON`. It only kept passing because the test binary
  // had not been relinked since the flag flipped; a genuine solver-ON `ctest`
  // fails it.
  auto const asked = dispatch(fx, {"groups", "recommend", "1", "--solver", "mtkahypar", "--json"});
  CHECK(asked.code == 0);

  if (::planar::engine::grouping::mtkahypar::available()) {
    // The solver is linked: the optimal arm really ran. `selected_greedy`
    // still reports whether greedy WON on cost — the never-worse-than-greedy
    // comparison in `grouping/load.cpp` — so it is not asserted here.
    CHECK(asked.out.contains("\"solver\":\"mtkahypar\""));
    CHECK(asked.out.contains("\"optimal_available\":true"));
  } else {
    // No solver linked: degrade to greedy and SAY SO. This is byte-identical
    // to what the oracle produces on a host with no solver installed.
    CHECK(asked.out.contains("\"solver\":\"greedy\""));
    CHECK(asked.out.contains("\"optimal_available\":false"));
    CHECK(asked.out.contains("\"selected_greedy\":false"));

    // ...and the answer is identical to an explicit `--solver greedy`, which
    // is what "degraded" means. This equality holds ONLY on the degraded arm:
    // with the solver linked the two differ in `solver`/`optimal_available`
    // by construction.
    auto const greedy = dispatch(fx, {"groups", "recommend", "1", "--solver", "greedy", "--json"});
    CHECK(greedy.code == 0);
    CHECK(greedy.out == asked.out);
  }
}
