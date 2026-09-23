// @file sync_status_leaves.t.cpp
// @brief In-process tests for `planar-ext sync status`. Moved from
// `planar`'s `plan_descendants_sync_status_leaves.t.cpp` at plan 996, task
// 6419 — `sync status` now lives here; `plan descendants` (the other half
// of that file, plan 996 task 6298) stayed on `planar` and its tests
// stayed with it.
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
// ## NO PLAN/TASK TREE HERE, UNLIKE THE ORIGINAL FILE
//
// `sync status` reads `external_links` (optionally joined to
// `external_systems` for `--system`) and nothing else — `link::list`
// carries no join to `tasks`/`plans`/`questions`, confirmed by reading
// `src/lib/engine/external/link.cpp`. The original file's `seed_tree` (a
// plan/task tree for the SIBLING `plan descendants` cases) was therefore
// never load-bearing for these three cases; it stayed on `planar` with
// `plan descendants` and is not reproduced here.
//
// ## ORACLE PROVENANCE
//
// Every expected byte was captured from `zig/zig-out/bin/planar` in a pinned
// scratch arena. The captures that decided a shape:
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
import planar.db.migrate;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.dispatch;
import planar.cmd.planar_ext.main;

namespace {

using planar::cmd::ext::context;

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
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_ext_6298_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Migrate the fixture database directly — `planar-ext` has no
/// `init` verb. See `ext_leaves.t.cpp`'s `migrate_fixture` for the full
/// account.
void migrate_fixture(const fixture& fx) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));
}

/// @brief Dispatch `args` against the real tree and table.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar-ext"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::ext::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::ext::database>(fx.db_path, err), out, err};
  auto const         tree  = planar::cmd::ext::root_app();
  auto const         table = planar::cmd::ext::handlers(*tree);
  int const          code  = planar::cmd::ext::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run one statement against the fixture database, failing loudly.
void exec(planar::db::connection& conn, std::string_view sql) {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// @brief Count rows in one table.
auto count(planar::db::connection& conn, std::string_view table) -> std::int64_t {
  auto stmt = conn.prepare(std::format("select count(*) from {}", table));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Seed two systems and four links for the `sync status` cases.
///
/// SQL rather than the CLI: `ext create` (the only verb that writes an
/// `external_links` row) is unported, and `ext register` lives on this
/// same binary but `tasks`/`plans`/`questions` rows do not exist here at
/// all — `sync status` needs none of them (see this file's header). The
/// set is chosen so that every rendering branch is REACHED: two links
/// carry a `last_synced_at` and two are NULL (the key is omitted from
/// JSON, and the text column reads `never`); all four `last_sync_status`
/// values appear; two systems and three entity kinds make `--system` and
/// `--entity` discriminate rather than match everything.
void seed_links(const fixture& fx) {
  migrate_fixture(fx);
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

TEST_CASE("the sync-status fixture seeds the links its cases read", "[cmd][sync][status][fixture]") {
  // Deliberately first and deliberately about nothing else — see the
  // header on `plan_descendants_sync_status_leaves.t.cpp` for why this
  // discipline exists.
  auto const fx = make_fixture("fixture");
  seed_links(fx);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "external_links") == 4);
  CHECK(count(*conn, "external_systems") == 2);
  auto stamped = conn->prepare("select count(*) from external_links where last_synced_at is not null");
  REQUIRE(stamped.has_value());
  REQUIRE(stamped->step().value() == planar::db::step_result::row);
  CHECK(stamped->column_int64(0) == 2);
}

TEST_CASE("sync status lists every link when unfiltered", "[cmd][sync][status]") {
  auto const fx = make_fixture("list");
  seed_links(fx);

  auto const text = dispatch(fx, {"sync", "status"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // ALIGNED AT TASK 6308. The `question:1` row used to push the rest of the
  // line right, because only the ID carried the column width (`{}:{:<11}`)
  // while the header promised `{:<14}`. Reproduced from the oracle under D2
  // until decision 1067 ended that rule; the entity column is now padded as
  // ONE field.
  //
  // `question:1` is what makes this case evidence: every other row here has a
  // four-character kind, so a fixture of only `task:`/`plan:` rows would line
  // up under both the old formatting and the new.
  CHECK(text.out == "link    entity          external-id         system    last-sync                 status\n"
                    "1       task:1          PROJ-11             1         2026-01-02T03:04:05.678Z  ok\n"
                    "2       task:2          o/r#7               2         never                     never\n"
                    "3       plan:1          PROJ-12             1         2026-02-03T04:05:06.789Z  conflict\n"
                    "4       question:1      o/r#8               2         never                     error\n");

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

TEST_CASE("sync status filters by system and by entity, and excludes by a survivor", "[cmd][sync][status]") {
  auto const fx = make_fixture("filter");
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

TEST_CASE("sync status treats an unknown system as empty and a bad entity as an error", "[cmd][sync][status]") {
  auto const fx = make_fixture("empty");
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
