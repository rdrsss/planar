// @file store.t.cpp
// @brief Unit tests for `planar.engine.closure.store` (plan 996, task 6095).
//
// ORACLE PROVENANCE. Every byte below was captured by RUNNING the Zig binary
// against a scratch database with FIXED timestamps, never from `--help` and
// never from reading the Zig source:
//
//   export PLANAR_DB=/tmp/fx/p.db PLANAR_CONFIG_PATH=/tmp/fx/p.toml
//   ./zig/zig-out/bin/planar init
//   sqlite3 /tmp/fx/p.db "
//     insert into closures (id,task_id,repo_id,path,symbol,role,token_weight,
//                           extractor_version,created_at) values
//       (1,1,1,'z/last.zig','a.aaa','reference', 5,'v1','2020-01-01T00:00:00.000Z'),
//       (2,1,1,'a/first.zig','z.zzz','modify',   7,'v1','2020-01-01T00:00:00.000Z'),
//       (3,1,1,'z/last.zig','a.aaa','modify',    5,'v1','2020-01-01T00:00:00.000Z'),
//       (4,1,1,'a/first.zig','z.zzz','transitive',7,'v2','2020-01-01T00:00:00.000Z'),
//       (5,1,1,'m/mid.zig','m.mmm','transitive', 6,'v1','2020-01-01T00:00:00.000Z'),
//       (6,1,1,'m/mid.zig','m.mmm','modify',     6,'v1','2020-01-01T00:00:00.000Z');"
//
// HAZARD 4 -- this fixture is deliberately a CONSTRUCTED TIE. Path order and
// symbol order disagree on purpose (`a/first.zig::z.zzz` sorts first by path
// and last by symbol), so the capture below discriminates path-first from
// symbol-first ordering, and insertion order from both.
//
//   $Z closure show 1 --json
//     -> {"task_id":1,"rows":[
//          {"id":2,...,"path":"a/first.zig","symbol":"z.zzz","role":"modify",...},
//          {"id":6,...,"path":"m/mid.zig","symbol":"m.mmm","role":"modify",...},
//          {"id":3,...,"path":"z/last.zig","symbol":"a.aaa","role":"modify",...},
//          {"id":1,...,"path":"z/last.zig","symbol":"a.aaa","role":"reference",...},
//          {"id":4,...,"path":"a/first.zig","symbol":"z.zzz","role":"transitive",...},
//          {"id":5,...,"path":"m/mid.zig","symbol":"m.mmm","role":"transitive",...}]}
//                          ^ ids run 2,6,3,1,4,5 -- not insertion order
//
//   $Z closure show 1
//     -> 'closure for task 1 (6 rows):'
//        '  [modify] a/first.zig::z.zzz  w=7'
//        '  [modify] m/mid.zig::m.mmm  w=6'
//        '  [modify] z/last.zig::a.aaa  w=5'
//        '  [reference] z/last.zig::a.aaa  w=5'
//        '  [transitive] a/first.zig::z.zzz  w=7'
//        '  [transitive] m/mid.zig::m.mmm  w=6'
//
// HAZARD 2 -- empty input. An unknown task id is NOT an error:
//
//   $Z closure show 999 --json -> exit 0, {"task_id":999,"rows":[]}
//   $Z closure show 999        -> exit 0,
//        'closure for task 999 (0 rows):'
//        '  (none \xe2\x80\x94 run `planar closure compute 999` first)'   [em dash]
//   $Z closure show notanint --json -> exit 2,
//        error: task id must be an integer, got 'notanint'
//   $Z closure show -5 --json -> exit 2, error: unknown flag (got -5)
//        [a negative id never reaches the leaf -- the PARSER eats it as a
//         flag, so there is no negative-id behavior to port]
//
// ============================================================================
// DEFERRED LEAF HANDOFF: `closure compute`
// ============================================================================
// Not ported (tree-sitter + a filesystem corpus walk + a cross-bucket handler
// -- see store.cppm's cut list for why all three are disqualifying). Its
// oracle behavior is captured HERE so whoever picks it up starts from
// evidence rather than from the Zig source. Fixture: a repo with
// src/a.zig (`const b = @import("b.zig"); pub fn alpha() void { b.beta(); }
// pub fn gamma() u32 { return 1; }`) and src/b.zig (`pub fn beta() void {}
// pub fn delta() void {}`), task 1 declaring src/a.zig, task 2 declaring
// src/b.zig.
//
//   $Z closure compute 1 --json
//     -> {"task_id":1,"seeds":1,"modify":3,"reference":2,"transitive":0,
//         "rows_written":5,"extractor_version":"m2-closure-0.1"}
//   $Z closure compute 1
//     -> 'closure computed for task 1: 1 seed(s) -> 5 rows
//         (modify=3 reference=2 transitive=0); extractor=m2-closure-0.1'
//   $Z closure compute 2 --json
//     -> {"task_id":2,"seeds":1,"modify":2,"reference":0,"transitive":0,
//         "rows_written":2,"extractor_version":"m2-closure-0.1"}
//   $Z closure compute 999 --json   -> exit 1, error: no task with id 999
//   $Z closure compute notanint --json
//                                   -> exit 2, error: task id must be an
//                                      integer, got 'notanint'
//   $Z closure compute <task-with-no-touches> --json
//     -> exit 2, error: closure compute: task 1 declares no path-level
//        touches (task_touch_paths); nothing to compute
//   $Z closure compute 1 --scope global --json
//     -> exit 5, error: scope mismatch: task 1 is in scope 'project:repo'
//        but operator write scope is 'global'; pass --scope project:repo to
//        write to that scope from here
//        [THIS is the cross-bucket reach -- the scope guard lives in
//         engine.identity, a layer-2 peer]
//
// Extracted symbol semantics, for whoever ports the extractor: task 1's
// closure was `modify` {a.alpha, a.b, a.gamma} and `reference` {a.b, b.beta}
// -- note `a.b`, the import binding, appears under BOTH roles, and the file
// stem prefixes the symbol. Recompute is a REPLACEMENT, not an append:
// running `closure compute 1` twice leaves 5 rows with fresh ids, not 10.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.closure.store;

namespace {

namespace cs = planar::engine::closure::store;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_closure_test_{}_{}.db",
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

/// @brief Reproduce the constructed-tie fixture from this file's header.
auto seed_tie_fixture(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id,slug,name,root_path) values (1,'repo','repo','/tmp/fx/repo')");
  exec(conn, "insert into plans (id,scope_kind,title,slug,status) values (1,'global','P','p','draft')");
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status) values "
             "(1,'global',1,'T','t','todo')");
  exec(conn, "insert into closures (id,task_id,repo_id,path,symbol,role,token_weight,"
             "extractor_version,created_at) values "
             "(1,1,1,'z/last.zig','a.aaa','reference',5,'v1','2020-01-01T00:00:00.000Z'),"
             "(2,1,1,'a/first.zig','z.zzz','modify',7,'v1','2020-01-01T00:00:00.000Z'),"
             "(3,1,1,'z/last.zig','a.aaa','modify',5,'v1','2020-01-01T00:00:00.000Z'),"
             "(4,1,1,'a/first.zig','z.zzz','transitive',7,'v2','2020-01-01T00:00:00.000Z'),"
             "(5,1,1,'m/mid.zig','m.mmm','transitive',6,'v1','2020-01-01T00:00:00.000Z'),"
             "(6,1,1,'m/mid.zig','m.mmm','modify',6,'v1','2020-01-01T00:00:00.000Z')");
}

constexpr std::string_view k_show_json =
    R"({"task_id":1,"rows":[)"
    R"({"id":2,"repo_id":1,"path":"a/first.zig","symbol":"z.zzz","role":"modify","token_weight":7,)"
    R"("extractor_version":"v1","created_at":"2020-01-01T00:00:00.000Z"},)"
    R"({"id":6,"repo_id":1,"path":"m/mid.zig","symbol":"m.mmm","role":"modify","token_weight":6,)"
    R"("extractor_version":"v1","created_at":"2020-01-01T00:00:00.000Z"},)"
    R"({"id":3,"repo_id":1,"path":"z/last.zig","symbol":"a.aaa","role":"modify","token_weight":5,)"
    R"("extractor_version":"v1","created_at":"2020-01-01T00:00:00.000Z"},)"
    R"({"id":1,"repo_id":1,"path":"z/last.zig","symbol":"a.aaa","role":"reference","token_weight":5,)"
    R"("extractor_version":"v1","created_at":"2020-01-01T00:00:00.000Z"},)"
    R"({"id":4,"repo_id":1,"path":"a/first.zig","symbol":"z.zzz","role":"transitive","token_weight":7,)"
    R"("extractor_version":"v2","created_at":"2020-01-01T00:00:00.000Z"},)"
    R"({"id":5,"repo_id":1,"path":"m/mid.zig","symbol":"m.mmm","role":"transitive","token_weight":6,)"
    R"("extractor_version":"v1","created_at":"2020-01-01T00:00:00.000Z"}]})";

constexpr std::string_view k_show_text = "closure for task 1 (6 rows):\n"
                                         "  [modify] a/first.zig::z.zzz  w=7\n"
                                         "  [modify] m/mid.zig::m.mmm  w=6\n"
                                         "  [modify] z/last.zig::a.aaa  w=5\n"
                                         "  [reference] z/last.zig::a.aaa  w=5\n"
                                         "  [transitive] a/first.zig::z.zzz  w=7\n"
                                         "  [transitive] m/mid.zig::m.mmm  w=6\n";

} // namespace

TEST_CASE("closure.store: show orders by role, then PATH, then symbol", "[closure]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_tie_fixture(conn);

  auto rows = cs::show(conn, 1);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 6);

  // The discriminating sequence. Insertion order was 1,2,3,4,5,6; symbol
  // order within the modify group would be a.aaa, m.mmm, z.zzz. Neither
  // matches -- path order does.
  const std::vector<std::int64_t> expected_ids{2, 6, 3, 1, 4, 5};
  std::vector<std::int64_t>       actual_ids;
  for (const auto& r : *rows) {
    actual_ids.push_back(r.id);
  }
  REQUIRE(actual_ids == expected_ids);

  // Roles group modify -> reference -> transitive.
  REQUIRE((*rows)[0].role == "modify");
  REQUIRE((*rows)[2].role == "modify");
  REQUIRE((*rows)[3].role == "reference");
  REQUIRE((*rows)[4].role == "transitive");

  // ...and within the modify group, paths ascend while symbols descend.
  REQUIRE((*rows)[0].path == "a/first.zig");
  REQUIRE((*rows)[0].symbol == "z.zzz");
  REQUIRE((*rows)[2].path == "z/last.zig");
  REQUIRE((*rows)[2].symbol == "a.aaa");
}

TEST_CASE("closure.store: show returns rows from every extractor version together", "[closure]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_tie_fixture(conn);

  // Row id 4 carries extractor_version 'v2' while the rest carry 'v1'. Both
  // come back, undistinguished -- `show` does not filter by version even
  // though the UNIQUE key includes it, and `compute`'s delete-then-insert
  // only clears its OWN version.
  auto rows = cs::show(conn, 1);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 6);
  REQUIRE(std::ranges::count_if(*rows, [](const cs::row& r) { return r.extractor_version == "v2"; }) == 1);
}

TEST_CASE("closure.store: show scopes to the requested task", "[closure]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_tie_fixture(conn);
  exec(conn, "insert into tasks (id,scope_kind,plan_id,title,slug,status) values "
             "(2,'global',1,'T2','t2','todo')");
  exec(conn, "insert into closures (id,task_id,repo_id,path,symbol,role,token_weight,"
             "extractor_version,created_at) values "
             "(7,2,1,'a/first.zig','other.sym','modify',1,'v1','2020-01-01T00:00:00.000Z')");

  auto first = cs::show(conn, 1);
  REQUIRE(first.has_value());
  REQUIRE(first->size() == 6);

  auto second = cs::show(conn, 2);
  REQUIRE(second.has_value());
  REQUIRE(second->size() == 1);
  REQUIRE((*second)[0].symbol == "other.sym");
}

TEST_CASE("closure.store: an unknown task is an EMPTY result, not an error", "[closure]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Oracle-confirmed exit 0 on a completely empty database. The leaf never
  // checks that the task exists -- which is why the rendered envelope echoes
  // the requested id rather than reading it back.
  auto rows = cs::show(conn, 999);
  REQUIRE(rows.has_value());
  REQUIRE(rows->empty());
}

TEST_CASE("closure.store: show --json matches the oracle byte for byte", "[closure]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_tie_fixture(conn);

  auto rows = cs::show(conn, 1);
  REQUIRE(rows.has_value());
  REQUIRE(cs::render_show_json(1, *rows) == k_show_json);
}

TEST_CASE("closure.store: show text matches the oracle byte for byte", "[closure]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_tie_fixture(conn);

  auto rows = cs::show(conn, 1);
  REQUIRE(rows.has_value());
  REQUIRE(cs::render_show_text(1, *rows) == k_show_text);
}

TEST_CASE("closure.store: the empty renderers echo the REQUESTED id", "[closure]") {
  // Both envelopes carry 999 despite no row anywhere mentioning it -- proof
  // the id is echoed from the argument, not derived from the result set.
  REQUIRE(cs::render_show_json(999, {}) == R"({"task_id":999,"rows":[]})");
  // Em dash, not a hyphen; and the hint names the exact command including the
  // id.
  REQUIRE(cs::render_show_text(999, {}) == "closure for task 999 (0 rows):\n"
                                           "  (none \xe2\x80\x94 run `planar closure compute 999` first)\n");
  // The empty JSON is `"rows":[]` -- not `null`, not an absent key.
  REQUIRE(cs::render_show_json(1, {}).find(R"("rows":[])") != std::string::npos);
}

TEST_CASE("closure.store: the text row form uses TWO spaces before w=", "[closure]") {
  const std::vector<cs::row> rows{
      cs::row{.id                = 1,
              .task_id           = 1,
              .repo_id           = 1,
              .path              = "p.zig",
              .symbol            = "s.sym",
              .role              = "modify",
              .token_weight      = 42,
              .extractor_version = "v1",
              .created_at        = "2020-01-01T00:00:00.000Z"},
  };
  const auto out = cs::render_show_text(1, rows);
  REQUIRE(out == "closure for task 1 (1 rows):\n"
                 "  [modify] p.zig::s.sym  w=42\n");
  // Stated as a property so a single-space regression fails loudly: the
  // header line is "1 rows", not "1 row" -- the count is never pluralised.
  REQUIRE(out.find("  w=42") != std::string::npos);
  REQUIRE(out.find("(1 rows)") != std::string::npos);
}

TEST_CASE("closure.store: JSON escaping applies to path, symbol, and role", "[closure]") {
  const std::vector<cs::row> rows{
      cs::row{.id                = 1,
              .task_id           = 1,
              .repo_id           = 1,
              .path              = "a\"b\\c",
              .symbol            = "s\tsym",
              .role              = "modify",
              .token_weight      = 1,
              .extractor_version = "v\n1",
              .created_at        = "2020-01-01T00:00:00.000Z"},
  };
  const auto out = cs::render_show_json(1, rows);
  REQUIRE(out.find(R"("path":"a\"b\\c")") != std::string::npos);
  REQUIRE(out.find(R"("symbol":"s\tsym")") != std::string::npos);
  REQUIRE(out.find(R"("extractor_version":"v\n1")") != std::string::npos);
}

TEST_CASE("closure.store: the invalid-id message is NOT leaf-prefixed", "[closure]") {
  // Almost every other diagnostic in this port carries a `<leaf>: ` prefix.
  // This one deliberately does not -- captured as a bare
  // `error: task id must be an integer, got 'notanint'`.
  REQUIRE(cs::render_invalid_task_id("notanint") == "task id must be an integer, got 'notanint'");
  REQUIRE(cs::render_invalid_task_id("") == "task id must be an integer, got ''");
  REQUIRE(cs::render_invalid_task_id("notanint").find("closure show") == std::string::npos);
}
