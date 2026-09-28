// @file load.t.cpp
// @brief Unit tests for `planar.engine.grouping.load` and
// `planar.engine.grouping.greedy` (plan 996, task 6095).
//
// ORACLE PROVENANCE. Every slice, cost, and byte below was captured by RUNNING
// the Zig binary against seeded scratch databases, never from `--help` and
// never from reading the Zig source. All five fixtures were designed to
// DISCRIMINATE a specific rule rather than merely to be non-empty:
//
//   mkdir -p /tmp/gx/repo && cd /tmp/gx/repo && git init -q .
//   export PLANAR_DB=/tmp/gx/p.db PLANAR_CONFIG_PATH=/tmp/gx/p.toml
//   ./zig/zig-out/bin/planar init
//
// ---- FIXTURE 1 (plan 1): D-HG1 write-write, transitive exclusion, `done` ---
//   tasks 1,2,3 todo + task 4 done, all on plan 1.
//   closures: 1{shared.sym ref 10, ww.sym mod 50, t1.only mod 1}
//             2{shared.sym ref 10, t2.only mod 2}
//             3{ww.sym mod 50, t3.only mod 3, trans.sym TRANSITIVE 999}
//             4{done.sym mod 7}
//
//   $Z groups recommend 1 --json
//   -> {"plan_id":1,"budget":128000,"open_tasks":3,"solver":"greedy",
//       "optimal_available":false,"selected_greedy":false,
//       "slices":[{"task_ids":[1,2],
//                  "union_symbols":["shared.sym","t1.only","t2.only","ww.sym"],
//                  "cost":63},
//                 {"task_ids":[3],"union_symbols":["t3.only","ww.sym"],
//                  "cost":53}],
//       "summary":{"slices":2,"total_cost":116}}
//
//   Three rules proven at once: task 3 did NOT merge with task 1 despite
//   sharing ww.sym at weight 50 (write-write earns zero credit) while task 2
//   DID merge on shared.sym at weight 10; task 3's cost is 53, not 1052, so
//   trans.sym was excluded; open_tasks is 3 and done.sym appears nowhere.
//
//   $Z groups recommend 1
//   -> 'plan:1  budget:128000  open:3  solver:greedy  optimal_available:false
//       selected_greedy:false  slices:2  total_cost:116'
//      'slice 1  cost:63  tasks:[1, 2]'
//      '    - shared.sym' ... '    - ww.sym'
//      'slice 2  cost:53  tasks:[3]' ...
//
// ---- FIXTURE 2 (plan 2): empty input ------------------------------------
//   $Z groups recommend 2 --json
//   -> {"plan_id":2,...,"open_tasks":0,...,"slices":[],
//       "summary":{"slices":0,"total_cost":0}}      [exit 0, NOT not-found]
//   $Z groups recommend 2
//   -> header line, then '  (no open tasks to group)'
//
// ---- FIXTURE 3 (plan 3): a zero-overlap merge along a dependency ---------
//   tasks 10,11,12 with fully disjoint closures {d1.only 5},{d2.only 6},
//   {d3.only 7}; entity_links ('task',11,'task',10,'depends-on').
//
//   $Z groups recommend 3 --json
//   -> "slices":[{"task_ids":[10,11],"union_symbols":["d1.only","d2.only"],
//                 "cost":11},
//                {"task_ids":[12],"union_symbols":["d3.only"],"cost":7}]
//
//   10 and 11 merged with ZERO overlap purely because an edge joins them; 12,
//   with neither overlap nor edge, stayed alone.
//
// ---- FIXTURE 5 (plan 5): HAZARD 4, a CONSTRUCTED TIE --------------------
//   tasks 30{x:10}, 31{x:10, p:90}, 32{x:10, q:90}, --budget 100.
//   merge(30,31) and merge(30,32) BOTH score 10 and both fit at exactly 100;
//   merge(31,32) would cost 190 and does not. So the budget permits exactly
//   ONE of the tied merges and the tie-break becomes observable.
//
//   $Z groups recommend 5 --budget 100 --json
//   -> "slices":[{"task_ids":[30,31],"union_symbols":["p","x"],"cost":100},
//                {"task_ids":[32],"union_symbols":["q","x"],"cost":100}]
//
//   The reversed tie-break (larger tie_max wins) would have produced [30,32]
//   and [31]. At --budget 190 all three merge into one slice at cost 190,
//   confirming the budget is what isolated the tie.
//
// ---- Budget edges --------------------------------------------------------
//   $Z groups recommend 1 --budget 12 --json
//   -> three singletons; task 1 comes back ALONE at cost 61, which is over
//      the budget. A lone over-budget task is reported as-is, never dropped.
//   $Z groups recommend 1 --budget 0 --json   -> byte-identical to --budget 12
//      apart from the echoed budget.
//
// ---- Errors --------------------------------------------------------------
//   $Z groups recommend 99 --json   -> exit 1, error: plan 99 not found
//                                      [NOT quoted -- test-spec status quotes
//                                       its argument, this leaf does not]
//   $Z groups recommend nope --json -> exit 2,
//        error: plan id must be an integer, got 'nope'
//   $Z groups recommend 1 --budget notanint --json -> exit 2,
//        error: --budget must be a non-negative integer, got 'notanint'
//   $Z groups recommend 1 --budget -5 --json -> exit 2, same message with '-5'
//   $Z groups recommend 1 --solver bogus --json -> exit 2,
//        error: --solver must be 'greedy' or 'mtkahypar', got 'bogus'
//
// ---- The mtkahypar arm, and why nothing here pins it --------------------
//   $Z groups recommend 2 --solver mtkahypar --json   [on THIS machine, which
//                                                      has the solver]
//   -> {"plan_id":2,...,"solver":"mtkahypar","optimal_available":true,
//       "selected_greedy":false,
//       "slices":[{"task_ids":[3,4,5],"union_symbols":[...],"cost":30}],
//       "summary":{"slices":1,"total_cost":30}}
//   ...versus greedy's three singletons on the same input. On a machine
//   without the binary the identical command degrades silently to greedy with
//   "optimal_available":false. Either outcome would make a test pass or fail
//   on what happens to be installed, so the arm is not ported and not pinned
//   (see CMakeLists.txt's cut list).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.grouping.greedy;
import planar.engine.grouping.load;
import planar.engine.grouping.mtkahypar;

namespace {

namespace gg = planar::engine::grouping::greedy;
namespace gl = planar::engine::grouping::load;
namespace gm = planar::engine::grouping::mtkahypar;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_grouping_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

/// @brief FIXTURE 1 -- write-write conflict, transitive exclusion, and a
/// `done` task that must not participate.
auto seed_fixture_1(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values "
             "(1,'global','Grouping','g','active'),(2,'global','Empty','e','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(1,'global',1,'T1','t1','todo',100),"
             "(2,'global',1,'T2','t2','todo',100),"
             "(3,'global',1,'T3','t3','todo',100),"
             "(4,'global',1,'T4 done','t4','done',100)");
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(1,1,'p.zig','shared.sym','reference',10,'v1'),"
             "(1,1,'p.zig','ww.sym','modify',50,'v1'),"
             "(1,1,'p.zig','t1.only','modify',1,'v1'),"
             "(2,1,'p.zig','shared.sym','reference',10,'v1'),"
             "(2,1,'p.zig','t2.only','modify',2,'v1'),"
             "(3,1,'p.zig','ww.sym','modify',50,'v1'),"
             "(3,1,'p.zig','t3.only','modify',3,'v1'),"
             "(3,1,'p.zig','trans.sym','transitive',999,'v1'),"
             "(4,1,'p.zig','done.sym','modify',7,'v1')");
}

/// @brief FIXTURE 3 -- three mutually disjoint tasks, one dependency edge.
auto seed_fixture_dep(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (3,'global','Dep','d','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(10,'global',3,'D1','d1','todo',100),"
             "(11,'global',3,'D2','d2','todo',100),"
             "(12,'global',3,'D3','d3','todo',100)");
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(10,1,'p.zig','d1.only','modify',5,'v1'),"
             "(11,1,'p.zig','d2.only','modify',6,'v1'),"
             "(12,1,'p.zig','d3.only','modify',7,'v1')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',11,'task',10,'depends-on')");
}

/// @brief FIXTURE 6 (task 6460) -- forces the mtkahypar `k` estimate to
/// split a pair greedy would happily keep merged, so the never-worse
/// comparison in `recommend_with` has a REAL losing solver result to catch,
/// not just a tie.
///
/// Two independent sharing pairs (A,B) via `h` weight 90, (C,D) via `l`
/// weight 5, plus a disjoint task E with a large solo symbol (weight 200,
/// held by no one else). Deduped union total = 90+5+200 = 295. At
/// `--budget 90`: each pair's OWN merge cost fits the budget (90 and 5), so
/// GREEDY merges both pairs unconstrained by `k` -- 3 slices, total 295
/// (`{A,B}=90 + {C,D}=5 + {E}=200`). `block_count` computes
/// `ceil(295/90)=4`, forcing the solver into 4 non-empty blocks across 5
/// vertices -- pigeonhole means at least one pair must split, duplicating
/// its shared symbol and landing at total >= 300. Without the
/// never-worse comparison this seam would regress `groups recommend
/// --solver mtkahypar` below the greedy baseline on a plausible input.
auto seed_fixture_forced_split(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (6,'global','ForcedSplit','fs','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(40,'global',6,'A','fsa','todo',100),"
             "(41,'global',6,'B','fsb','todo',100),"
             "(42,'global',6,'C','fsc','todo',100),"
             "(43,'global',6,'D','fsd','todo',100),"
             "(44,'global',6,'E','fse','todo',100)");
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(40,1,'p.zig','h','reference',90,'v1'),"
             "(41,1,'p.zig','h','reference',90,'v1'),"
             "(42,1,'p.zig','l','reference',5,'v1'),"
             "(43,1,'p.zig','l','reference',5,'v1'),"
             "(44,1,'p.zig','e.only','modify',200,'v1')");
}

/// @brief FIXTURE 5 -- a constructed score tie the budget isolates.
auto seed_fixture_tie(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (5,'global','TieBreak','tb','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(30,'global',5,'A','tka','todo',100),"
             "(31,'global',5,'B','tkb','todo',100),"
             "(32,'global',5,'C','tkc','todo',100)");
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(30,1,'p.zig','x','reference',10,'v1'),"
             "(31,1,'p.zig','x','reference',10,'v1'),"
             "(31,1,'p.zig','p','modify',90,'v1'),"
             "(32,1,'p.zig','x','reference',10,'v1'),"
             "(32,1,'p.zig','q','modify',90,'v1')");
}

constexpr std::string_view k_fixture1_json =
    R"({"plan_id":1,"budget":128000,"open_tasks":3,"solver":"greedy","optimal_available":false,)"
    R"("selected_greedy":false,"slices":[)"
    R"({"task_ids":[1,2],"union_symbols":["shared.sym","t1.only","t2.only","ww.sym"],"cost":63},)"
    R"({"task_ids":[3],"union_symbols":["t3.only","ww.sym"],"cost":53}],)"
    R"("summary":{"slices":2,"total_cost":116}})"
    "\n";

constexpr std::string_view k_fixture1_text = "plan:1  budget:128000  open:3  solver:greedy  optimal_available:false  "
                                             "selected_greedy:false  slices:2  total_cost:116\n"
                                             "slice 1  cost:63  tasks:[1, 2]\n"
                                             "    - shared.sym\n"
                                             "    - t1.only\n"
                                             "    - t2.only\n"
                                             "    - ww.sym\n"
                                             "slice 2  cost:53  tasks:[3]\n"
                                             "    - t3.only\n"
                                             "    - ww.sym\n";

constexpr std::string_view k_empty_json =
    R"({"plan_id":2,"budget":128000,"open_tasks":0,"solver":"greedy","optimal_available":false,)"
    R"("selected_greedy":false,"slices":[],"summary":{"slices":0,"total_cost":0}})"
    "\n";

constexpr std::string_view k_empty_text = "plan:2  budget:128000  open:0  solver:greedy  optimal_available:false  "
                                          "selected_greedy:false  slices:0  total_cost:0\n"
                                          "  (no open tasks to group)\n";

} // namespace

TEST_CASE("grouping: a write-write symbol earns ZERO overlap credit", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto rec = gl::recommend(conn, 1, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices.size() == 2);

  // The discriminating outcome: tasks 1 and 3 share `ww.sym` at weight 50 --
  // FIVE TIMES the weight tasks 1 and 2 share -- yet 1+2 merged and 3 stayed
  // alone. A plain weighted-overlap heuristic would have merged 1+3 first.
  REQUIRE(rec->grouping_.slices[0].task_ids == std::vector<std::int64_t>{1, 2});
  REQUIRE(rec->grouping_.slices[0].cost == 63);
  REQUIRE(rec->grouping_.slices[1].task_ids == std::vector<std::int64_t>{3});
  REQUIRE(rec->grouping_.slices[1].cost == 53);
  REQUIRE(rec->grouping_.total_cost() == 116);
}

TEST_CASE("grouping: transitive closure rows are excluded from the effective closure", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  // Task 3 carries a `transitive` symbol of weight 999. If it leaked into the
  // effective closure the slice would cost 1052 and `trans.sym` would appear
  // in union_symbols.
  auto units = gl::load_units(conn, 3);
  REQUIRE(units.has_value());
  REQUIRE(units->size() == 2);
  REQUIRE(std::ranges::none_of(*units, [](const gg::unit& u) { return u.qualified == "trans.sym"; }));

  auto rec = gl::recommend(conn, 1, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices[1].cost == 53);
  REQUIRE(rec->grouping_.slices[1].union_symbols == std::vector<std::string>{"t3.only", "ww.sym"});
}

TEST_CASE("grouping: only status='todo' tasks are candidates", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto ids = gl::load_open_task_ids(conn, 1);
  REQUIRE(ids.has_value());
  REQUIRE(*ids == std::vector<std::int64_t>{1, 2, 3});

  // `doing` and `blocked` are excluded exactly like `done` -- the filter is
  // `= 'todo'`, not "not terminal". Driving task 2 to `doing` must drop it.
  exec(conn, "update tasks set status = 'doing' where id = 2");
  auto after = gl::load_open_task_ids(conn, 1);
  REQUIRE(after.has_value());
  REQUIRE(*after == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("grouping: open tasks are ordered by priority then id", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);
  // Give the highest-id task the lowest priority number so priority and id
  // order disagree; priority must win.
  exec(conn, "update tasks set priority = 1 where id = 3");

  auto ids = gl::load_open_task_ids(conn, 1);
  REQUIRE(ids.has_value());
  REQUIRE(*ids == std::vector<std::int64_t>{3, 1, 2});
}

TEST_CASE("grouping: a zero-overlap pair merges only along a dependency edge", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_dep(conn);

  auto rec = gl::recommend(conn, 3, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices.size() == 2);
  // 10 and 11 share NOTHING; the edge is the entire reason they co-locate.
  REQUIRE(rec->grouping_.slices[0].task_ids == std::vector<std::int64_t>{10, 11});
  REQUIRE(rec->grouping_.slices[0].cost == 11);
  // 12 has neither overlap nor an edge, so it is not fused in.
  REQUIRE(rec->grouping_.slices[1].task_ids == std::vector<std::int64_t>{12});
  REQUIRE(rec->grouping_.slices[1].cost == 7);
}

TEST_CASE("grouping: removing the dependency edge leaves three singletons", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_dep(conn);
  exec(conn, "delete from entity_links");

  // The inverse of the previous case: with no edge and no overlap, nothing
  // merges. This is what proves the edge (not some incidental adjacency) is
  // doing the work there.
  auto rec = gl::recommend(conn, 3, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices.size() == 3);
  REQUIRE(rec->grouping_.total_cost() == 18);
}

TEST_CASE("grouping: load_deps flips the edge orientation", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_dep(conn);

  // `entity_links` says from_id=11 depends on to_id=10. `greedy::dep` is the
  // OPPOSITE orientation, so the produced edge must be
  // {blocked = 11, blocker = 10}. A flipped mapping would produce
  // {blocked = 10, blocker = 11} and would make the cycle check reason about
  // a mirrored DAG.
  const std::vector<std::int64_t> open{10, 11, 12};
  auto                            deps = gl::load_deps(conn, open);
  REQUIRE(deps.has_value());
  REQUIRE(deps->size() == 1);
  REQUIRE((*deps)[0].blocked == 11);
  REQUIRE((*deps)[0].blocker == 10);
}

TEST_CASE("grouping: load_deps drops edges leaving the open set, and self-edges", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_dep(conn);
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',10,'task',999,'depends-on')," // to a task outside the set
             "('task',12,'task',12,'depends-on')"); // self-edge

  const std::vector<std::int64_t> open{10, 11, 12};
  auto                            deps = gl::load_deps(conn, open);
  REQUIRE(deps.has_value());
  // Only the original in-set edge survives.
  REQUIRE(deps->size() == 1);
  REQUIRE((*deps)[0].blocked == 11);

  // ...and narrowing the open set drops that one too.
  const std::vector<std::int64_t> narrowed{11, 12};
  auto                            fewer = gl::load_deps(conn, narrowed);
  REQUIRE(fewer.has_value());
  REQUIRE(fewer->empty());
}

TEST_CASE("grouping: a symbol under BOTH roles folds to ONE unit, at the max weight", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (1,'global','P','p','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(1,'global',1,'T','t','todo',100)");
  // Same symbol under both roles with DIFFERENT weights. The reference row is
  // the heavier one deliberately, so the fold's "larger weight wins" arm
  // fires on the SECOND row -- see the note below on which row is second.
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(1,1,'p.zig','dual','reference',9,'v1'),"
             "(1,1,'p.zig','dual','modify',3,'v1')");

  auto units = gl::load_units(conn, 1);
  REQUIRE(units.has_value());
  // Two rows, ONE unit -- the fold happened.
  REQUIRE(units->size() == 1);
  REQUIRE((*units)[0].qualified == "dual");
  // modify dominates, so the write-conflict detection can see the symbol...
  REQUIRE((*units)[0].role_ == gg::role::modify);
  // ...and the larger of the two weights is kept, whichever role carried it.
  REQUIRE((*units)[0].weight == 9);
}

TEST_CASE("grouping: the dual-role fold is observable as a write-write non-merge", "[grouping]") {
  // The end-to-end consequence of the fold above, and the reason it matters:
  // if a dual-role symbol folded to `reference` instead of `modify`, the pair
  // below would score 10 and merge. Oracle-confirmed that it does NOT:
  //
  //   task 50 lists 'd' under BOTH roles; task 51 lists it as modify only.
  //   $Z groups recommend 7 --json
  //     -> "slices":[{"task_ids":[50],"union_symbols":["d"],"cost":10},
  //                  {"task_ids":[51],"union_symbols":["d"],"cost":10}],
  //        "summary":{"slices":2,"total_cost":20}
  //
  // NOTE on the fold's modify-dominance arm, found by break-probe. Mutating
  // `if (r == modify) out[..].role_ = modify` to `if (false)` SURVIVES, and
  // that is not a gap in this test -- the arm is genuinely unreachable under
  // the query's plan. `explain query plan` on
  //   select ... where task_id = ? and role in ('modify','reference')
  //          order by symbol
  // reports `SEARCH closures USING INDEX ix_closures_task_role (task_id=? AND
  // role=?)` plus a temp b-tree for the ORDER BY: the IN-list drives two
  // index seeks in sorted order, so 'modify' rows always arrive BEFORE
  // 'reference' rows for a given task, and the stable symbol sort preserves
  // that. The unit is therefore always CREATED as modify and the dominance
  // arm never has to upgrade it. It is defensive in the Zig original too and
  // is kept for the same reason: it is what makes the fold correct
  // independently of the query planner's choice. The behaviour these tests
  // pin -- one unit, max weight, role modify -- holds either way.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (7,'global','Fold','fo','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(50,'global',7,'A','foa','todo',100),"
             "(51,'global',7,'B','fob','todo',100)");
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(50,1,'p.zig','d','reference',10,'v1'),"
             "(50,1,'p.zig','d','modify',10,'v1'),"
             "(51,1,'p.zig','d','modify',10,'v1')");

  auto rec = gl::recommend(conn, 7, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices.size() == 2);
  REQUIRE(rec->grouping_.slices[0].task_ids == std::vector<std::int64_t>{50});
  REQUIRE(rec->grouping_.slices[1].task_ids == std::vector<std::int64_t>{51});
  REQUIRE(rec->grouping_.total_cost() == 20);
}

TEST_CASE("grouping: HAZARD 4 -- ties break toward the smaller second member", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_tie(conn);

  // merge(30,31) and merge(30,32) both score 10 and both land at exactly 100;
  // merge(31,32) would cost 190. The budget therefore permits exactly one of
  // the tied merges, which is what makes the choice observable at all.
  auto rec = gl::recommend(conn, 5, 100);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices.size() == 2);
  REQUIRE(rec->grouping_.slices[0].task_ids == std::vector<std::int64_t>{30, 31});
  REQUIRE(rec->grouping_.slices[0].cost == 100);
  REQUIRE(rec->grouping_.slices[1].task_ids == std::vector<std::int64_t>{32});
  // The reversed tie-break would give {30,32} and {31} -- same slice count and
  // same total cost, so only the MEMBERSHIP discriminates.
  REQUIRE(rec->grouping_.slices[1].union_symbols == std::vector<std::string>{"q", "x"});

  // Raising the budget lets both merges happen, confirming the budget (not
  // some ordering accident) is what isolated the tie.
  auto roomy = gl::recommend(conn, 5, 190);
  REQUIRE(roomy.has_value());
  REQUIRE(roomy->grouping_.slices.size() == 1);
  REQUIRE(roomy->grouping_.slices[0].task_ids == std::vector<std::int64_t>{30, 31, 32});
  REQUIRE(roomy->grouping_.slices[0].cost == 190);
}

TEST_CASE("grouping: the budget blocks a merge but never shrinks a lone task", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto rec = gl::recommend(conn, 1, 12);
  REQUIRE(rec.has_value());
  REQUIRE(rec->grouping_.slices.size() == 3);
  // Task 1's own closure costs 61 -- five times the budget -- and it comes
  // back intact rather than dropped or truncated. The budget constrains
  // MERGING, not membership.
  REQUIRE(rec->grouping_.slices[0].task_ids == std::vector<std::int64_t>{1});
  REQUIRE(rec->grouping_.slices[0].cost == 61);
  REQUIRE(rec->grouping_.total_cost() == 126);

  // A budget of zero behaves identically -- it is not a special case.
  auto zero = gl::recommend(conn, 1, 0);
  REQUIRE(zero.has_value());
  REQUIRE(zero->grouping_.slices.size() == 3);
  REQUIRE(zero->grouping_.total_cost() == 126);
}

TEST_CASE("grouping: an existing plan with no open tasks succeeds with zero slices", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  // HAZARD 2. Plan 2 exists and has nothing to group. That is exit 0 with an
  // empty slices array -- NOT not_found, NOT null.
  auto rec = gl::recommend(conn, 2, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->open_tasks == 0);
  REQUIRE(rec->grouping_.slices.empty());
  REQUIRE(rec->grouping_.total_cost() == 0);
  REQUIRE(gl::render_json(*rec) == k_empty_json);
  REQUIRE(gl::render_text(*rec) == k_empty_text);
}

TEST_CASE("grouping: a missing plan is not_found, distinct from an empty plan", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto missing = gl::recommend(conn, 99, gl::k_default_budget);
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error() == gl::grouping_error::not_found);
}

TEST_CASE("grouping: --json output matches the oracle byte for byte", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto rec = gl::recommend(conn, 1, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(gl::render_json(*rec) == k_fixture1_json);
}

TEST_CASE("grouping: text output matches the oracle byte for byte", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto rec = gl::recommend(conn, 1, gl::k_default_budget);
  REQUIRE(rec.has_value());
  const auto out = gl::render_text(*rec);
  REQUIRE(out == k_fixture1_text);
  // The two forms deliberately differ in their task-id separator: the text
  // form uses ", " and the JSON form a bare ",".
  REQUIRE(out.find("tasks:[1, 2]") != std::string::npos);
  REQUIRE(gl::render_json(*rec).find(R"("task_ids":[1,2])") != std::string::npos);
}

TEST_CASE("grouping: the default budget is the literal 128000 in the envelope", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto rec = gl::recommend(conn, 1, gl::k_default_budget);
  REQUIRE(rec.has_value());
  // Not merely documented in help text -- it is echoed into both outputs.
  REQUIRE(gl::render_json(*rec).find(R"("budget":128000)") != std::string::npos);
  REQUIRE(gl::render_text(*rec).find("budget:128000  ") != std::string::npos);
}

TEST_CASE("grouping: the reporting flags stay false without the optimal arm", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto rec = gl::recommend(conn, 1, gl::k_default_budget);
  REQUIRE(rec.has_value());
  REQUIRE(rec->solver_ == gl::solver::greedy);
  // `optimal_available` reports whether the mtkahypar arm RAN, and
  // `selected_greedy` whether it ran and lost. Both false here is the same
  // shape a machine without the solver binary reports -- which is why neither
  // flag can distinguish "not ported" from "not installed".
  REQUIRE_FALSE(rec->optimal_available);
  REQUIRE_FALSE(rec->selected_greedy);
  REQUIRE(gl::render_json(*rec).find(R"("optimal_available":false,"selected_greedy":false)") != std::string::npos);
}

TEST_CASE("grouping: recommend_with(solver::greedy) is identical to recommend", "[grouping]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto plain = gl::recommend(conn, 1, gl::k_default_budget);
  auto via   = gl::recommend_with(conn, 1, gl::k_default_budget, gl::solver::greedy);
  REQUIRE(plain.has_value());
  REQUIRE(via.has_value());
  REQUIRE(gl::render_json(*plain) == gl::render_json(*via));
}

TEST_CASE("grouping: recommend_with(solver::mtkahypar) degrades to greedy when the seam is unavailable (task 6460)",
          "[grouping]") {
  // This test binary is built WITHOUT -DPLANAR_WITH_MTKAHYPAR=ON in every
  // default `cmake --build`, so `mtkahypar::available()` is false and this
  // is the branch that actually runs under `ctest -L engine_grouping`. The
  // check is gated on `available()` rather than hard-coded so the SAME test
  // source stays correct (rather than spuriously failing) when compiled
  // against a `-DPLANAR_WITH_MTKAHYPAR=ON` build where the seam IS linked --
  // exercised manually against the real library while developing this seam
  // (see this task's break-probe log for that run).
  if (gm::available()) {
    SUCCEED("built with -DPLANAR_WITH_MTKAHYPAR=ON; degrade path is not reachable here, see the tie-fixture test instead");
    return;
  }

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_1(conn);

  auto greedy_rec = gl::recommend(conn, 1, gl::k_default_budget);
  auto mtk_rec    = gl::recommend_with(conn, 1, gl::k_default_budget, gl::solver::mtkahypar);
  REQUIRE(greedy_rec.has_value());
  REQUIRE(mtk_rec.has_value());
  REQUIRE(mtk_rec->solver_ == gl::solver::greedy);
  REQUIRE_FALSE(mtk_rec->optimal_available);
  REQUIRE_FALSE(mtk_rec->selected_greedy);
  REQUIRE(gl::render_json(*mtk_rec) == gl::render_json(*greedy_rec));
}

TEST_CASE("grouping: recommend_with(solver::mtkahypar) is never worse than greedy (task 4247)", "[grouping]") {
  // Runs ONLY when the real seam is linked in (`-DPLANAR_WITH_MTKAHYPAR=ON`);
  // a default build has nothing to prove here beyond the degrade test above,
  // so it SUCCEED()s trivially rather than skipping silently.
  if (!gm::available()) {
    SUCCEED("built without -DPLANAR_WITH_MTKAHYPAR=ON; the real comparison needs the linked seam");
    return;
  }

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_tie(conn);

  // budget:100 forces block_count() to 2 (deduped union x+p+q = 190), so the
  // solver arm actually partitions rather than trivially merging everything.
  auto greedy_rec = gl::recommend(conn, 5, 100);
  auto mtk_rec    = gl::recommend_with(conn, 5, 100, gl::solver::mtkahypar);
  REQUIRE(greedy_rec.has_value());
  REQUIRE(mtk_rec.has_value());

  // The contract itself: the SAME cost function, and the shipped result is
  // never worse than greedy's own.
  REQUIRE(mtk_rec->grouping_.total_cost() <= greedy_rec->grouping_.total_cost());
  REQUIRE(mtk_rec->solver_ == gl::solver::mtkahypar);
  REQUIRE(mtk_rec->optimal_available);
  // `selected_greedy` records which arm's grouping actually shipped -- when
  // set, the shipped cost must equal greedy's own (not merely be <= it),
  // proving the fallback ships greedy's ACTUAL result rather than some other
  // value that happens to satisfy the inequality.
  if (mtk_rec->selected_greedy) {
    REQUIRE(mtk_rec->grouping_.total_cost() == greedy_rec->grouping_.total_cost());
  }
}

TEST_CASE("grouping: recommend_with(solver::mtkahypar) never regresses below greedy even when the solver's OWN "
          "partition would (task 6460 fixture)",
          "[grouping]") {
  // Runs ONLY when the real seam is linked in. See seed_fixture_forced_split
  // for why this input is expected to give the raw solver a genuinely worse
  // partition than greedy: block_count's k=4 pigeonholes a split that
  // greedy's own budget check never forces. This is the test that would
  // catch an "always accept mtkahypar" regression -- the tie fixture above
  // cannot, because both arms tie there.
  if (!gm::available()) {
    SUCCEED("built without -DPLANAR_WITH_MTKAHYPAR=ON; the real comparison needs the linked seam");
    return;
  }

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_fixture_forced_split(conn);

  auto greedy_rec = gl::recommend(conn, 6, 90);
  auto mtk_rec    = gl::recommend_with(conn, 6, 90, gl::solver::mtkahypar);
  REQUIRE(greedy_rec.has_value());
  REQUIRE(mtk_rec.has_value());

  // Greedy achieves the true optimum here (295): both pairs merge, nothing
  // is ever forced apart.
  REQUIRE(greedy_rec->grouping_.total_cost() == 295);

  // The contract: whatever the raw solver call produced internally, the
  // SHIPPED result is never worse than greedy's 295.
  REQUIRE(mtk_rec->grouping_.total_cost() <= 295);
  REQUIRE(mtk_rec->optimal_available);
  REQUIRE(mtk_rec->solver_ == gl::solver::mtkahypar);
  // Not asserted as a hard requirement (a future solver version could find
  // 295 directly, which would also satisfy the contract above) but WARN
  // records what actually happened for this run: measured empirically while
  // developing this seam, the raw solver call DOES produce a worse partition
  // here (block_count's k=4 pigeonholes a split) and `selected_greedy` flips
  // to true, proving the comparison -- not the solver -- is what keeps this
  // test green. See this task's break-probe log.
  WARN("selected_greedy=" << mtk_rec->selected_greedy << " shipped_cost=" << mtk_rec->grouping_.total_cost());
}

TEST_CASE("grouping: solver parsing accepts exactly two values", "[grouping]") {
  REQUIRE(gl::solver_from_text("greedy") == gl::solver::greedy);
  REQUIRE(gl::solver_from_text("mtkahypar") == gl::solver::mtkahypar);
  REQUIRE_FALSE(gl::solver_from_text("bogus").has_value());
  REQUIRE_FALSE(gl::solver_from_text("").has_value());
  REQUIRE_FALSE(gl::solver_from_text("Greedy").has_value());

  REQUIRE(gl::solver_to_text(gl::solver::greedy) == "greedy");
  REQUIRE(gl::solver_to_text(gl::solver::mtkahypar) == "mtkahypar");
}

TEST_CASE("grouping: the diagnostic strings match the oracle verbatim", "[grouping]") {
  // Note the plan id is UNQUOTED here, while `test-spec status` quotes its
  // argument. The two leaves genuinely differ and a shared helper would get
  // one of them wrong.
  REQUIRE(gl::render_plan_not_found(99) == "plan 99 not found");
  REQUIRE(gl::render_plan_not_found(1) == "plan 1 not found");
  REQUIRE(gl::render_invalid_plan_id("nope") == "plan id must be an integer, got 'nope'");
  REQUIRE(gl::render_invalid_budget("notanint") == "--budget must be a non-negative integer, got 'notanint'");
  REQUIRE(gl::render_invalid_budget("-5") == "--budget must be a non-negative integer, got '-5'");
  // This one carries no leaf prefix at all.
  REQUIRE(gl::render_invalid_solver("bogus") == "--solver must be 'greedy' or 'mtkahypar', got 'bogus'");
  REQUIRE(gl::render_invalid_solver("bogus").find("groups recommend") == std::string::npos);
}

TEST_CASE("grouping: D-HG2 refuses a merge that would make the slice-DAG cyclic", "[grouping]") {
  // FIXTURE 6 (plan 6). Isolating the cycle refusal takes more care than it
  // looks: an A->B->C chain where A and C share a symbol does NOT keep A and
  // C apart on its own, because greedy merges A into B first (a zero-overlap
  // dependency merge) and then folds C into that same slice, which is
  // trivially schedulable. The first version of this test asserted exactly
  // that and was WRONG -- it failed, and the fixture was rebuilt rather than
  // the assertion relaxed.
  //
  // The refusal only becomes observable when B cannot absorb anyone: give B a
  // closure heavier than the budget so every merge INVOLVING B is refused on
  // cost, leaving the A+C merge as the only candidate -- and that one is
  // refused on ordering. Oracle-confirmed:
  //
  //   $Z groups recommend 6 --budget 100 --json
  //   -> "slices":[{"task_ids":[40],...,"cost":10},
  //                {"task_ids":[41],"union_symbols":["heavy"],"cost":1000},
  //                {"task_ids":[42],...,"cost":10}]
  //      "summary":{"slices":3,"total_cost":1020}
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'r','r','/tmp/gx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (6,'global','Cycle','cy','active')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status,priority) values "
             "(40,'global',6,'A','cya','todo',100),"
             "(41,'global',6,'B','cyb','todo',100),"
             "(42,'global',6,'C','cyc','todo',100)");
  exec(conn, "insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) values "
             "(40,1,'p.zig','hot','reference',10,'v1'),"
             "(41,1,'p.zig','heavy','modify',1000,'v1'),"
             "(42,1,'p.zig','hot','reference',10,'v1')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',41,'task',40,'depends-on'),"  // 40 precedes 41
             "('task',42,'task',41,'depends-on')"); // 41 precedes 42

  auto constrained = gl::recommend(conn, 6, 100);
  REQUIRE(constrained.has_value());
  REQUIRE(constrained->grouping_.slices.size() == 3);
  REQUIRE(constrained->grouping_.slices[0].task_ids == std::vector<std::int64_t>{40});
  REQUIRE(constrained->grouping_.slices[1].task_ids == std::vector<std::int64_t>{41});
  REQUIRE(constrained->grouping_.slices[2].task_ids == std::vector<std::int64_t>{42});
  REQUIRE(constrained->grouping_.total_cost() == 1020);

  // Now the control: delete ONLY the dependency chain, change nothing else.
  // 40 and 42 immediately merge on their shared symbol, which proves the
  // three-way split above came from the ordering constraint and not from the
  // budget or the overlap score. Also oracle-confirmed:
  //   -> "slices":[{"task_ids":[40,42],"union_symbols":["hot"],"cost":10},
  //                {"task_ids":[41],...,"cost":1000}]
  exec(conn, "delete from entity_links");
  auto unconstrained = gl::recommend(conn, 6, 100);
  REQUIRE(unconstrained.has_value());
  REQUIRE(unconstrained->grouping_.slices.size() == 2);
  REQUIRE(unconstrained->grouping_.slices[0].task_ids == std::vector<std::int64_t>{40, 42});
  REQUIRE(unconstrained->grouping_.slices[0].cost == 10);
  REQUIRE(unconstrained->grouping_.slices[1].task_ids == std::vector<std::int64_t>{41});
  REQUIRE(unconstrained->grouping_.total_cost() == 1010);
}

TEST_CASE("grouping.greedy: a dependency chain does NOT by itself keep two tasks apart", "[grouping]") {
  // The complement of the case above, pinned so nobody "fixes" the cycle
  // check into over-refusing. A -> B -> C with A and C sharing a heavy symbol
  // and a budget that fits everything collapses into ONE slice: A merges with
  // B along the edge, then C folds into the same slice, and a single slice
  // has no inter-slice ordering to violate.
  const std::vector<gg::task> tasks{
      gg::task{.id = 1, .units = {gg::unit{.qualified = "hot", .role_ = gg::role::reference, .weight = 100}}},
      gg::task{.id = 2, .units = {gg::unit{.qualified = "mid", .role_ = gg::role::modify, .weight = 1}}},
      gg::task{.id = 3, .units = {gg::unit{.qualified = "hot", .role_ = gg::role::reference, .weight = 100}}},
  };
  const std::vector<gg::dep> deps{
      gg::dep{.blocked = 2, .blocker = 1},
      gg::dep{.blocked = 3, .blocker = 2},
  };

  const auto out = gg::group(tasks, deps, 100000);
  REQUIRE(out.slices.size() == 1);
  REQUIRE(out.slices[0].task_ids == std::vector<std::int64_t>{1, 2, 3});
  REQUIRE(out.slices[0].cost == 101);
}

TEST_CASE("grouping.greedy: an empty task set yields an empty grouping", "[grouping]") {
  const auto out = gg::group({}, {}, 128000);
  REQUIRE(out.slices.empty());
  REQUIRE(out.total_cost() == 0);
}

TEST_CASE("grouping.greedy: union symbols and slice order are sorted, not hash order", "[grouping]") {
  // Symbols inserted in descending order and tasks presented out of id order.
  // Both must come back sorted: the union is materialized from a hash map, so
  // without the explicit sorts the output would vary between runs.
  const std::vector<gg::task> tasks{
      gg::task{.id    = 9,
               .units = {gg::unit{.qualified = "zzz", .role_ = gg::role::reference, .weight = 1},
                         gg::unit{.qualified = "mmm", .role_ = gg::role::reference, .weight = 1},
                         gg::unit{.qualified = "aaa", .role_ = gg::role::reference, .weight = 1}}},
      gg::task{.id = 2, .units = {gg::unit{.qualified = "solo", .role_ = gg::role::modify, .weight = 1}}},
  };
  const auto out = gg::group(tasks, {}, 128000);
  REQUIRE(out.slices.size() == 2);
  // Slices ordered by smallest member id: task 2's slice comes first.
  REQUIRE(out.slices[0].task_ids == std::vector<std::int64_t>{2});
  REQUIRE(out.slices[1].task_ids == std::vector<std::int64_t>{9});
  REQUIRE(out.slices[1].union_symbols == std::vector<std::string>{"aaa", "mmm", "zzz"});
}
