// @file ext_propagate_leaf.t.cpp
// @brief In-process tests for `planar-ext ext propagate` (plan 996, task
// 6421). This is the GitHub `parent-issue` arm only -- see
// handlers/propagate.cppm for the full scope note. Cases here exercise the
// CLI bridge end-to-end against a real in-process HTTP fixture server, the
// same shape `ext_propagate_one_leaf.t.cpp` and `ext_create_leaf.t.cpp` use.
// The engine-layer orchestration itself (child plans, tasks-under-plan,
// direct anchor tasks, the probe-then-create ordering) is ALREADY pinned by
// `parent_issue.t.cpp` against a stub `gh_client` with no network edge --
// this file's job is proving the CLI wiring: flag parsing, strategy
// selection, adapter construction, and rendering, all end to end through a
// real socket.
//
// NO REAL NETWORK: the fixture server binds 127.0.0.1:0 only, and every
// registered system's `base_url` is rewritten to point at it.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.dispatch;
import planar.cmd.planar_ext.tree;

// AFTER the imports -- see ext_propagate_one_leaf.t.cpp on why.
#include "../lib/http/fixture_server.hpp"

namespace {

using planar::cmd::ext::context;

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_ext_prop_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

void migrate_fixture(const fixture& fx) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar-ext"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::ext::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::ext::root_app();
  auto const         table = planar::cmd::ext::handlers(*tree);
  int const          code  = planar::cmd::ext::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

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

/// @brief One issue-create counter per repo -- used to hand out increasing
/// issue numbers exactly like a real GitHub repo would, so parent/child
/// sub-issue links reference numbers that actually came from a create
/// response rather than a fixed constant.
struct issue_counter {
  std::atomic<std::int64_t> next{100};
};

/// @brief The canned GitHub response set every happy-path case shares.
/// `sub_issue_probe_status` controls the probe's response
/// (`issues/0/sub_issues`); everything else is fixed shape.
auto make_respond(issue_counter& counter, int sub_issue_probe_status, bool fail_second_create = false)
    -> std::function<planar::http::fixture::canned_response(const planar::http::fixture::captured_request&)> {
  return [&counter, sub_issue_probe_status, fail_second_create,
          seen = std::make_shared<std::atomic<int>>(0)](const planar::http::fixture::captured_request& req)
             -> planar::http::fixture::canned_response {
    if (req.target.contains("/issues/0/sub_issues")) {
      return {.status = sub_issue_probe_status, .body = "{}", .content_type = "application/json"};
    }
    if (req.target.contains("/sub_issues")) {
      // Linking a real sub-issue.
      return {.status = 201, .body = "{}", .content_type = "application/json"};
    }
    if (req.target.contains("/comments")) {
      return {.status = 201, .body = "{}", .content_type = "application/json"};
    }
    if (req.verb == "POST" && req.target.ends_with("/issues")) {
      auto const count = seen->fetch_add(1) + 1;
      if (fail_second_create && count == 2) {
        return {.status = 500, .body = "internal error", .content_type = "text/plain"};
      }
      auto const number = counter.next.fetch_add(1);
      return {.status      = 201,
              .body        = std::format(R"({{"number":{},"node_id":"NODE_{}"}})", number, number),
              .content_type = "application/json"};
    }
    return {.status = 404, .body = "{}", .content_type = "application/json"};
  };
}

/// @brief Register a GitHub system pointed at `base`, and seed an anchor
/// plan, one child plan, one task under the child, and one task attached
/// directly to the anchor. The child-plan task carries `scope_kind='repo'`
/// pointing at a single seeded project, so both `select_strategy` and
/// `resolve_target_repo` see exactly one touched repo and agree on
/// `github-parent-issue`.
void seed_single_repo(const fixture& fx, std::string_view base) {
  migrate_fixture(fx);
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-demo", "--auth-env", "DEMO_TOKEN", "--project", "acme/widgets"})
              .code == 0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", base)).has_value());
  REQUIRE(conn->execute("insert into projects (id, slug, name) values (1, 'acme/widgets', 'acme/widgets')").has_value());
  REQUIRE(
      conn->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor plan', 'anchor-plan')")
          .has_value());
  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug, parent_plan_id) values "
                        "(2, 'global', 'Child plan', 'child-plan', 1)")
              .has_value());
  REQUIRE(conn->execute("insert into tasks (id, scope_kind, scope_id, plan_id, title) values "
                        "(1, 'repo', 1, 2, 'Task under child')")
              .has_value());
  REQUIRE(conn->execute("insert into tasks (id, scope_kind, plan_id, title) values (2, 'global', 1, 'Direct anchor task')")
              .has_value());
}

} // namespace

TEST_CASE("ext propagate fixture sanity: seeded rows are present and no links exist yet",
          "[cmd][ext][propagate][fixture]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 201));
  auto const                    fx = make_fixture("fixture");
  seed_single_repo(fx, remote.base_url());

  CHECK(scalar(fx, "select count(*) from plans") == 2);
  CHECK(scalar(fx, "select count(*) from tasks") == 2);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
  CHECK(remote.request_count() == 0);
}

TEST_CASE("ext propagate creates the parent issue, both sub-issues, and records mirror links",
          "[cmd][ext][propagate][happy]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422)); // 422 on the bogus probe id == supported.
  auto const                    fx = make_fixture("happy");
  seed_single_repo(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out.contains(R"("strategy":"github-parent-issue")"));
  CHECK(ran.out.contains(R"("created":4)")); // anchor + child plan + 2 tasks
  CHECK(ran.out.contains(R"("skipped":0)"));
  CHECK(ran.out.contains(R"("failed":0)"));
  CHECK(ran.out.contains(R"("entity_kind":"plan")"));
  CHECK(ran.out.contains(R"("entity_kind":"task")"));

  // One mirror link per created entity: anchor plan, child plan, 2 tasks.
  CHECK(scalar(fx, "select count(*) from external_links") == 4);
  CHECK(scalar(fx, "select count(*) from external_links where link_role = 'mirror'") == 4);
  // The anchor's link carries the strategy-stickiness cache with the
  // resolved repo and parent issue number.
  CHECK(scalar(fx, "select count(*) from external_links where entity_kind = 'plan' and entity_id = 1 and "
                  "config_json like '%github-parent-issue%'") == 1);
  // One probe + 4 creates (anchor, child plan, task-under-child,
  // direct-anchor-task) + 3 sub-issue links (every non-anchor entity links
  // under its parent) = 8 requests. (No comments: no decisions were seeded.)
  CHECK(remote.request_count() == 8);
}

TEST_CASE("a repeated ext propagate SKIPS every entity and sends only the probe", "[cmd][ext][propagate][idempotent]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("idem");
  seed_single_repo(fx, remote.base_url());

  auto const first = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"});
  REQUIRE(first.code == 0);
  REQUIRE(first.out.contains(R"("created":4)"));
  auto const first_requests = remote.request_count();
  REQUIRE(first_requests > 0);

  auto const again = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"});
  CHECK(again.code == 0);
  CHECK(again.out.contains(R"("created":0)"));
  CHECK(again.out.contains(R"("skipped":4)"));
  CHECK(again.out.contains(R"("failed":0)"));

  // THE ASSERTION THIS CASE EXISTS FOR: the repeat sends only the sub-issue
  // support probe (which is not cached across a fresh process the way the
  // oracle's own run-1/run-2/run-3 trace requires -- see
  // `detect_parent_issue_support`'s header) and zero creates or links.
  CHECK(remote.request_count() == first_requests + 1);
  CHECK(scalar(fx, "select count(*) from external_links") == 4);
}

TEST_CASE("ext propagate refuses with sub_issue_unsupported when the probe 404s, before creating anything",
          "[cmd][ext][propagate][sub-issue-404]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 404)); // endpoint not enabled for this account.
  auto const                    fx = make_fixture("probe404");
  seed_single_repo(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo"});
  CHECK(ran.code == 2); // invalid_input
  CHECK(ran.err.contains("does not expose the sub-issue REST endpoint"));
  CHECK(ran.err.contains("tracking-issue"));

  // Nothing created: the probe runs before step 1 (the anchor issue).
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
  CHECK(remote.request_count() == 1); // the probe alone.
}

TEST_CASE("ext propagate reports a partial completion when one entity's create fails mid-propagation",
          "[cmd][ext][propagate][partial-failure]") {
  issue_counter                 counter;
  // The SECOND create call (the child plan, per propagate_parent_issue_with_repo's
  // step order: anchor first, then the child plan) fails with a transport
  // error; the direct anchor task afterward still gets attempted.
  planar::http::fixture::server remote(make_respond(counter, 422, /*fail_second_create=*/true));
  auto const                    fx = make_fixture("partial");
  seed_single_repo(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"});
  // failed > 0 maps to invalid_input, same as the oracle's exit.die.
  CHECK(ran.code == 2);
  CHECK(ran.out.contains(R"("failed":1)"));
  CHECK(ran.out.contains(R"("op":"failed")"));
  CHECK(ran.err.contains("entity/entities failed during propagation"));

  // The anchor (created before the failure) and whatever survived after it
  // still recorded links; the failed entity recorded none. Partial
  // completion, not an all-or-nothing rollback -- matches the oracle, which
  // has no transaction wrapping this whole walk either.
  CHECK(scalar(fx, "select count(*) from external_links") < 4);
  CHECK(scalar(fx, "select count(*) from external_links") > 0);
}

TEST_CASE("ext propagate --dry-run builds no adapter, sends nothing, and reports op:created with (dry-run) ids",
          "[cmd][ext][propagate][dry-run]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("dry");
  seed_single_repo(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--dry-run", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out.contains(R"js("external_id":"(dry-run)")js"));
  CHECK(ran.out.contains(R"("created":4)"));
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate refuses a Jira system explicitly rather than mis-executing the parent-issue path",
          "[cmd][ext][propagate][jira-refusal]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("jira");
  migrate_fixture(fx);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor', 'anchor')")
              .has_value());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "jira-demo"});
  CHECK(ran.code == 2);
  CHECK(ran.err.contains("jira-epic"));
  CHECK(ran.err.contains("not yet implemented in planar-ext"));
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate refuses a multi-repo GitHub feature by name (projects-v2 was cut, decision 1001)",
          "[cmd][ext][propagate][multi-repo-refusal]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("multirepo");
  seed_single_repo(fx, remote.base_url());
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  // A second repo touched via the direct anchor task (task id 2).
  REQUIRE(conn->execute("insert into projects (id, slug, name) values (2, 'acme/other', 'acme/other')").has_value());
  REQUIRE(conn->execute("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                        "values ('task', 2, 'repo', 2, 'touches')")
              .has_value());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo"});
  CHECK(ran.code == 2);
  CHECK(ran.err.contains("multiple repos"));
  CHECK(ran.err.contains("decision 1001"));
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate refuses an unknown plan and an unknown system before building anything",
          "[cmd][ext][propagate][refusal][ordering]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("refuse");
  seed_single_repo(fx, remote.base_url());

  auto const bad_plan = dispatch(fx, {"ext", "propagate", "999", "--system", "gh-demo"});
  CHECK(bad_plan.code == 1); // not_found
  CHECK(bad_plan.err.contains("plan '999' not found"));

  auto const bad_system = dispatch(fx, {"ext", "propagate", "1", "--system", "nope"});
  CHECK(bad_system.code == 1);
  CHECK(bad_system.err.contains("external system 'nope' not found"));

  auto const bad_sync = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--sync", "sideways"});
  CHECK(bad_sync.code == 2);
  CHECK(bad_sync.err.contains("read-only, write-back, two-way"));

  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}
