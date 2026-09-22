// @file propagate_one_publish_leaves.t.cpp
// @brief In-process tests for `planar workbench publish` (plan 996, task
// 6335).
//
// `ext propagate-one` — the other half of this file through task 6419 —
// moved to `src/cmd/planar-ext/ext_propagate_one_leaf.t.cpp` at plan 996,
// task 6419 (the `ext`/`sync` family's move to `planar-ext`). Its systems
// are now seeded directly through the engine rather than through
// `planar::cmd::dispatch`, the same way `audit_publish_decision_leaf.t.cpp`
// seeds `jira-demo`/`gh-demo` — `ext register` no longer lives on this
// binary's dispatch table.
//
// ## THE FIRST CASE ASSERTS THE FIXTURE
//
// Every case here turns on a live fixture HTTP server and two registered
// systems whose `base_url` points at it. If the `gh-demo` URL rewrite failed,
// each case would fail at the transport and refuse identically — and a
// uniform failure is indistinguishable from a uniform refusal for any other
// reason, so a link-count assertion would pass for the wrong reason. The
// first case pins the server, both systems, the seeded entities and the
// EMPTY link table before any comparison runs. Same discipline as
// `ext_create_leaf.t.cpp` and the `tree` cycle before it.
//
// ## WHAT THE ORIGINAL CYCLE (TASK 6335) ACTUALLY MEASURED
//
// `workbench publish` was carried in the unported inventory under "the
// create/propagate half of `engine_extsync`" — 3665 lines across five
// files. Read by SYMBOL rather than by file name, it reaches 1 function, 36
// lines (`recordLink`). That is the same correction already recorded for
// `audit commits` (6272), the `sync` trio (6294) and `plan descendants`
// (6298).
//
// ## THE THIRD ANSWER TO "IT ALREADY EXISTS"
//
// `ext create` (moved to `planar-ext`) POSTs a SECOND ticket, then refuses
// on the duplicate link (defect 6313); `ext propagate-one` (also moved)
// SKIPS and sends nothing. `workbench publish` is the third shape again:
// it REFUSES on the existing link, before rendering.
//
// ## ORACLE PROVENANCE
//
// The payload is indent-2 JSON, not compact: `render.zig:52-57`'s `toJson`
// passes `.whitespace = .indent_2`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.engine.external;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

// AFTER the imports, not before — the header names `std::function` and
// `std::thread` without including <functional> or <thread> itself.
#include "../lib/http/fixture_server.hpp"

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
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_p1pub_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"PLANAR_WORKBENCH_ROOT", (root / "wb").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()},
                  {"DEMO_TOKEN", "tok-abc"}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table.
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

/// @brief Read one text column, or `<none>` when no row matched.
auto text(const fixture& fx, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  if (*step != planar::db::step_result::row) {
    return "<none>";
  }
  return std::string{stmt->column_text(0)};
}

/// @brief The canned create responses, one per provider.
///
/// Endpoint discrimination is itself pinned: a port that built the wrong URL
/// would land on the other provider's branch and return an id of the wrong
/// SHAPE rather than failing outright.
auto respond(const planar::http::fixture::captured_request& req) -> planar::http::fixture::canned_response {
  if (req.target.contains("rest/api/3/issue")) {
    return {.status = 201, .body = R"({"key":"DEMO-77"})", .content_type = "application/json"};
  }
  return {
      .status = 201, .body = R"({"number":42,"html_url":"https://example.invalid/i/42"})", .content_type = "application/json"};
}

/// @brief Register both providers against `base` and seed a plan and a task.
///
/// `ext register` moved to `planar-ext` at plan 996, task 6419 — seeded
/// directly through the engine here, same as `audit_publish_decision_leaf
/// .t.cpp`'s `seed_jira_demo`/`seed_github_demo`.
void seed(const fixture& fx, std::string_view base) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::engine::external::system::register_jira(
                *conn, {.slug = "jira-demo", .base_url = base, .project = "DEMO", .auth_env = "DEMO_TOKEN"})
                .has_value());
    REQUIRE(planar::engine::external::system::register_github(
                *conn, {.slug = "gh-demo", .project = "acme/widgets", .auth_env = "DEMO_TOKEN"})
                .has_value());
    // `register_github` always points at the real api.github.com; redirect
    // it here, which is what keeps this suite OFF the network.
    REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", base)).has_value());
  }
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor plan", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Demo task", "--plan", "1", "--json"}).code == 0);
}

} // namespace

TEST_CASE("workbench publish records a mirror link AND its sync event", "[cmd][workbench][publish]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("publish");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"workbench", "publish", "1", "--system", "jira-demo", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out.contains(R"("ok":true)"));
  CHECK(ran.out.contains(R"("external_id":"DEMO-77")"));
  CHECK(ran.out.contains(R"("link_id":1)"));

  CHECK(scalar(fx, "select count(*) from external_links") == 1);
  CHECK(text(fx, "select link_role from external_links where id = 1") == "mirror");
  CHECK(text(fx, "select sync_direction from external_links where id = 1") == "two-way");
  CHECK(text(fx, "select last_sync_status from external_links where id = 1") == "ok");

  // THE ASSERTION THAT SEPARATES `record_mirror_link` FROM `link::create`:
  // the audit event. `create` writes external_links and nothing else, so a
  // port that reused it would pass every check above and fail only here.
  CHECK(scalar(fx, "select count(*) from sync_events where link_id = 1 and direction = 'push' and outcome = 'ok'") == 1);
  // `fields_changed` and `detail` stay NULL — the oracle's three-column
  // insert, not `update_sync_direction`'s five-column one.
  CHECK(scalar(fx, "select count(*) from sync_events where link_id = 1 and fields_changed is null and detail is null") == 1);
}

TEST_CASE("workbench publish REFUSES a second publication rather than skipping it", "[cmd][workbench][publish][duplicate]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("dup");
  seed(fx, remote.base_url());

  REQUIRE(dispatch(fx, {"workbench", "publish", "1", "--system", "jira-demo", "--json"}).code == 0);
  REQUIRE(remote.request_count() == 1);

  auto const again = dispatch(fx, {"workbench", "publish", "1", "--system", "jira-demo", "--json"});
  // The THIRD of the three duplicate shapes: not `ext create`'s second POST,
  // not `propagate-one`'s silent skip — a refusal.
  CHECK(again.code != 0);
  CHECK(again.err.contains("already has an external link on jira-demo"));
  // And it refuses BEFORE sending: still 1.
  CHECK(remote.request_count() == 1);
  CHECK(scalar(fx, "select count(*) from external_links") == 1);
}

TEST_CASE("workbench publish refuses an unknown plan and an unknown system", "[cmd][workbench][publish][refusal]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("pubrefuse");
  seed(fx, remote.base_url());

  auto const no_plan = dispatch(fx, {"workbench", "publish", "999", "--system", "jira-demo"});
  CHECK(no_plan.code != 0);
  CHECK(no_plan.err.contains("plan not found: 999"));

  auto const no_system = dispatch(fx, {"workbench", "publish", "1", "--system", "nope"});
  CHECK(no_system.code != 0);
  CHECK(no_system.err.contains("external system 'nope' not found"));

  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}
