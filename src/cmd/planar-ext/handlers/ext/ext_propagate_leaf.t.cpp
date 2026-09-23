// @file ext_propagate_leaf.t.cpp
// @brief In-process tests for `planar-ext ext propagate` (plan 996, task
// 6421). This is the GitHub `parent-issue` arm only -- see
// handlers/ext/propagate.cppm for the full scope note. Cases here exercise the
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
import planar.cmd.planar_ext.main;

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
  context            ctx{std::move(argv), planar::cmd::ext::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::ext::database>(fx.db_path, err), out, err};
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

auto text_scalar(const fixture& fx, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return std::string{stmt->column_text(0)};
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
  return [&counter, sub_issue_probe_status, fail_second_create, seen = std::make_shared<std::atomic<int>>(0)](
             const planar::http::fixture::captured_request& req) -> planar::http::fixture::canned_response {
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
      return {.status       = 201,
              .body         = std::format(R"({{"number":{},"node_id":"NODE_{}"}})", number, number),
              .content_type = "application/json"};
    }
    return {.status = 404, .body = "{}", .content_type = "application/json"};
  };
}

/// @brief `make_respond` extended with a `GET .../issues/<n>` arm for
/// `--verify-counterparts`'s probe (`github_adapter::pull`). Any issue
/// number in `missing_numbers` reports 404 (`adapter_error::not_found` ->
/// probe outcome `missing`); every other pull reports 200 with an empty
/// issue body (outcome `present`). Falls through to `make_respond`'s shape
/// for every create/link/comment/sub-issue-probe request, so a fixture
/// built with this responder can run a full happy-path propagate AND a
/// subsequent verify pass against the same server.
auto make_respond_with_pull(issue_counter& counter, std::set<std::int64_t> missing_numbers)
    -> std::function<planar::http::fixture::canned_response(const planar::http::fixture::captured_request&)> {
  return [&counter, missing = std::move(missing_numbers)](
             const planar::http::fixture::captured_request& req) -> planar::http::fixture::canned_response {
    if (req.verb == "GET" && req.target.contains("/issues/") && !req.target.contains("sub_issues")) {
      auto const   pos     = req.target.rfind('/');
      auto const   num_str = req.target.substr(pos + 1);
      std::int64_t n       = 0;
      auto const   conv    = std::from_chars(num_str.data(), num_str.data() + num_str.size(), n);
      if (conv.ec == std::errc{} && missing.contains(n)) {
        return {.status = 404, .body = "{}", .content_type = "application/json"};
      }
      return {.status = 200, .body = "{}", .content_type = "application/json"};
    }
    return make_respond(counter, 422)(req);
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
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-demo", "--auth-env", "DEMO_TOKEN", "--project", "acme/widgets"}).code ==
          0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", base)).has_value());
  REQUIRE(conn->execute("insert into projects (id, slug, name) values (1, 'acme/widgets', 'acme/widgets')").has_value());
  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor plan', 'anchor-plan')")
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

TEST_CASE("ext propagate fixture sanity: seeded rows are present and no links exist yet", "[cmd][ext][propagate][fixture]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 201));
  auto const                    fx = make_fixture("fixture");
  seed_single_repo(fx, remote.base_url());

  CHECK(scalar(fx, "select count(*) from plans") == 2);
  CHECK(scalar(fx, "select count(*) from tasks") == 2);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
  CHECK(remote.request_count() == 0);
}

TEST_CASE("ext propagate creates the parent issue, both sub-issues, and records mirror links", "[cmd][ext][propagate][happy]") {
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

TEST_CASE("a repeated ext propagate SKIPS every entity and sends NOTHING", "[cmd][ext][propagate][idempotent]") {
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

  // THE ASSERTION THIS CASE EXISTS FOR: the repeat sends NOTHING AT ALL --
  // zero creates, zero links, and not even the sub-issue support probe.
  //
  // It used to send one request here, the probe, because the cache took
  // three propagates to hit (run 1's write no-opped against a row that did
  // not exist yet, and run 2's read missed because the strategy write had
  // replaced `config_json` wholesale). Task 6354 / decision 1126 fixed both
  // halves, so the cache answers on run 2 and this count went from
  // `first_requests + 1` to `first_requests`. That delta IS the fix,
  // measured end-to-end against a real HTTP fixture server rather than a
  // call-counting double -- one fewer round-trip against a provider on
  // every early propagate.
  CHECK(remote.request_count() == first_requests);
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
  issue_counter counter;
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

TEST_CASE("ext propagate runs the generic jira-epic tree walk end to end, no longer refusing (task 6451)",
          "[cmd][ext][propagate][jira-generic]") {
  // A Jira-shaped 201 response for every issue create -- the generic
  // per-entity loop (`propagate_one_entity`) is the SAME core
  // `propagate-one` uses, so this is the same shape
  // `ext_propagate_one_leaf.t.cpp`'s jira case exercises.
  planar::http::fixture::server remote([](const planar::http::fixture::captured_request&) {
    return planar::http::fixture::canned_response{
        .status = 201, .body = R"({"key":"DEMO-77"})", .content_type = "application/json"};
  });
  auto const                    fx = make_fixture("jira");
  migrate_fixture(fx);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor', 'anchor')").has_value());
  REQUIRE(conn->execute("insert into tasks (id, scope_kind, plan_id, title) values (1, 'global', 1, 'Demo task')").has_value());
  // `walk_tree` reaches a task only through a `derives-from` entity_links
  // row, NOT `tasks.plan_id` -- see `descendants.cppm`'s header.
  REQUIRE(conn->execute("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                        "values ('task', 1, 'plan', 1, 'derives-from')")
              .has_value());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "jira-demo", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out.contains(R"("strategy":"jira-epic")"));
  CHECK(ran.out.contains(R"("created":2)"));
  CHECK(ran.out.contains(R"("failed":0)"));
  CHECK(remote.request_count() == 2);
  CHECK(scalar(fx, "select count(*) from external_links") == 2);
  // Only the ANCHOR caches the strategy.
  CHECK(text_scalar(fx, "select config_json from external_links where entity_kind = 'plan' and entity_id = 1") ==
        R"({"strategy":"jira-epic"})");
  CHECK(text_scalar(fx, "select config_json from external_links where entity_kind = 'task' and entity_id = 1").empty());

  // A repeat run over the SAME feature is idempotent: every entity already
  // has a mirror link, so the generic loop's `propagate_one_entity` call
  // hits the idempotency gate and reports "skipped", sending no new
  // requests. This is the generic-loop counterpart of the dedicated
  // `parent_issue` engine's own "a repeated ext propagate SKIPS every
  // entity" case above -- the two paths share `propagate_one_entity`, but
  // nothing here proved that WITHOUT this second run.
  auto const repeat = dispatch(fx, {"ext", "propagate", "1", "--system", "jira-demo", "--json"});
  CHECK(repeat.code == 0);
  CHECK(repeat.out.contains(R"("created":0)"));
  CHECK(repeat.out.contains(R"("skipped":2)"));
  CHECK(repeat.out.contains(R"("op":"skipped")"));
  CHECK_FALSE(repeat.out.contains(R"("op":"created")"));
  CHECK(remote.request_count() == 2); // unchanged -- the repeat sent nothing new.
  CHECK(scalar(fx, "select count(*) from external_links") == 2);
}

TEST_CASE("ext propagate --dry-run on the generic jira-epic loop reports op:\"planned\", never \"failed\" (task 6451)",
          "[cmd][ext][propagate][jira-generic][dry-run]") {
  // Regression coverage for a real bug this cycle's own manual reproduction
  // caught (the fast Catch2 suite had NO case exercising a dry-run generic
  // loop, so this specific defect was invisible until the slow Zig oracle
  // parity suite's "reimplemented ext propagate yields same results as
  // iterating propagate-one" case failed): `op_text`'s switch had no arm
  // for the new `parent_issue::op::planned` enumerator, so every dry-run
  // row fell into the `failed`/`default` case and rendered `op:"failed"`
  // with a `"<template-kind>"` external_id -- the exact SHAPE of a planned
  // row, mislabeled. `remote.request_count() == 0` proves this is asserted
  // under dry-run, where no create ever reaches the network.
  planar::http::fixture::server remote(
      [](const planar::http::fixture::captured_request&) -> planar::http::fixture::canned_response {
        FAIL("dry-run must not contact the remote");
        return {.status = 500, .body = "{}", .content_type = "application/json"};
      });
  auto const fx = make_fixture("jira-dryplanned");
  migrate_fixture(fx);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor', 'anchor')").has_value());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "jira-demo", "--dry-run", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out.contains(R"("op":"planned")"));
  CHECK_FALSE(ran.out.contains(R"("op":"failed")"));
  CHECK(ran.out.contains(R"("external_id":"<epic>")"));
  CHECK(ran.out.contains(R"("failed":0)"));
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate --github-strategy overrides auto-selection: same fixture, two different outcomes (task 6451)",
          "[cmd][ext][propagate][github-strategy]") {
  // A zero-touched-repo GitHub feature auto-selects `github-zero-repo` (a
  // generic-loop bucket, task 6451) and succeeds by posting through the
  // SYSTEM's own registered project -- it never needs a touched repo at
  // all. `--github-strategy parent-issue` forces the DIFFERENT, dedicated
  // `parent_issue::propagate_parent_issue` engine instead, which DOES need
  // one -- so the identical fixture must diverge: auto succeeds, override
  // refuses. If the override were a no-op this pair would be identical.
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("ghstrategy");
  migrate_fixture(fx);
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-strat", "--auth-env", "DEMO_TOKEN", "--project", "acme/widgets"}).code ==
          0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-strat'", remote.base_url()))
              .has_value());
  REQUIRE(conn->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor', 'anchor')").has_value());

  // Auto-selected: `github-zero-repo`, a generic-loop bucket -- succeeds.
  auto const auto_ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-strat", "--json"});
  CHECK(auto_ran.code == 0);
  CHECK(auto_ran.out.contains(R"("strategy":"github-zero-repo")"));
  CHECK(scalar(fx, "select count(*) from external_links") == 1);
  REQUIRE(conn->execute("delete from external_links").has_value());

  // Same fixture, `--github-strategy parent-issue`: forces the dedicated
  // engine, which refuses on the missing touched repo instead.
  auto const override_ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-strat", "--github-strategy", "parent-issue"});
  CHECK(override_ran.code == 2);
  CHECK(override_ran.err.contains("needs a touched repo"));
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate --github-strategy projects-v2 refuses by decision 1001, never reaching the engine (task 6451)",
          "[cmd][ext][propagate][github-strategy]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("ghstratv2");
  seed_single_repo(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--github-strategy", "projects-v2"});
  CHECK(ran.code == 2);
  CHECK(ran.err.contains("multiple repos"));
  CHECK(ran.err.contains("decision 1001"));
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate --github-strategy validates its value, its system kind, and its exclusivity with --restrategize",
          "[cmd][ext][propagate][github-strategy]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("ghstratbad");
  seed_single_repo(fx, remote.base_url());

  auto const bad_value = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--github-strategy", "bogus"});
  CHECK(bad_value.code == 2);
  CHECK(bad_value.err.contains("invalid --github-strategy"));

  auto const both_flags =
      dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--github-strategy", "parent-issue", "--restrategize"});
  CHECK(both_flags.code == 2);
  CHECK(both_flags.err.contains("mutually exclusive"));

  // A Jira system, told to use a GitHub-only override.
  auto const fx2 = make_fixture("ghstratjira");
  migrate_fixture(fx2);
  REQUIRE(dispatch(fx2, {"ext", "register", "jira", "jira-x", "--base-url", "https://x.atlassian.net", "--project", "X",
                         "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  auto conn2 = planar::db::connection::open(fx2.db_path.string());
  REQUIRE(conn2.has_value());
  REQUIRE(conn2->execute("insert into plans (id, scope_kind, title, slug) values (1, 'global', 'Anchor', 'anchor')").has_value());
  auto const wrong_kind = dispatch(fx2, {"ext", "propagate", "1", "--system", "jira-x", "--github-strategy", "parent-issue"});
  CHECK(wrong_kind.code == 2);
  CHECK(wrong_kind.err.contains("--github-strategy is only valid for github-issues systems"));
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

// ---- --unlink / --recreate / --verify-counterparts preflight (task 6428) --

TEST_CASE("ext propagate refuses --unlink or --recreate without --verify-counterparts, before touching the network",
          "[cmd][ext][propagate][verify][refusal][ordering]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("verify-preflight");
  seed_single_repo(fx, remote.base_url());

  auto const unlink_alone = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--unlink"});
  CHECK(unlink_alone.code == 2);
  CHECK(unlink_alone.err.contains("--unlink and --recreate require --verify-counterparts"));

  auto const recreate_alone = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--recreate"});
  CHECK(recreate_alone.code == 2);
  CHECK(recreate_alone.err.contains("--unlink and --recreate require --verify-counterparts"));

  auto const both =
      dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--verify-counterparts", "--unlink", "--recreate"});
  CHECK(both.code == 2);
  CHECK(both.err.contains("--unlink and --recreate are mutually exclusive"));

  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("ext propagate --verify-counterparts reports every counterpart verified when the remote still has them",
          "[cmd][ext][propagate][verify][happy]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond_with_pull(counter, {}));
  auto const                    fx = make_fixture("verify-happy");
  seed_single_repo(fx, remote.base_url());

  REQUIRE(dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"}).code == 0);
  REQUIRE(scalar(fx, "select count(*) from external_links") == 4);

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--verify-counterparts", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out.contains(R"("ok":true)"));
  // All 4: task 6429/decision 1115 widened `list_mirror_links_in_tree` to
  // read `tasks.plan_id` as well as `entity_links(derives-from)`, matching
  // `descendants::walk_tree` (task 6307 fixed that function's identical
  // blind spot). `--verify-counterparts` now walks the same tree
  // `ext propagate` actually propagates, so every mirror link this fixture
  // seeded is reachable -- 2 plan-level links plus the 2 task links that
  // this fixture's tasks carry via `tasks.plan_id` alone.
  CHECK(ran.out.contains(R"("verified":4)"));
  CHECK_FALSE(ran.out.contains(R"("missing")"));
  CHECK(scalar(fx, "select count(*) from external_links") == 4);
  CHECK(scalar(fx, "select count(*) from sync_events where outcome = 'counterpart-missing'") == 0);
}

TEST_CASE("ext propagate --verify-counterparts reports a missing counterpart and refuses without --unlink/--recreate",
          "[cmd][ext][propagate][verify][missing]") {
  issue_counter counter;
  // Pull requests resolve after the happy propagate seeds real issue
  // numbers, so start with no missing numbers and patch the fixture's
  // knowledge in afterward via a fresh server pointed at the same set.
  auto const fx = make_fixture("verify-missing");
  {
    issue_counter                 seed_counter;
    planar::http::fixture::server seed_remote(make_respond_with_pull(seed_counter, {}));
    seed_single_repo(fx, seed_remote.base_url());
    REQUIRE(dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"}).code == 0);
  }
  REQUIRE(scalar(fx, "select count(*) from external_links") == 4);

  // The child plan's counterpart is the one the remote "deleted". Any
  // mirror link would do the same job now that `--verify-counterparts`
  // reaches every attachment shape (task 6429/decision 1115); the plan-level
  // link is used here because it is the simplest one to isolate.
  auto const missing_external_id =
      text_scalar(fx, "select external_id from external_links where entity_kind = 'plan' and entity_id = 2");
  auto const hash = missing_external_id.rfind('#');
  REQUIRE(hash != std::string::npos);
  std::int64_t const missing_number = std::stoll(missing_external_id.substr(hash + 1));

  planar::http::fixture::server remote2(make_respond_with_pull(counter, {missing_number}));
  // Re-point the registered system at the second server -- pull's base_url
  // is read fresh from `external_systems` on every invocation.
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", remote2.base_url()))
                .has_value());
  }

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--verify-counterparts", "--json"});
  CHECK(ran.code == 2); // invalid_input
  CHECK(ran.out.contains(R"("ok":false)"));
  CHECK(ran.out.contains(R"("missing":1)"));
  // 3 of the 4 seeded links verify; the child plan's is the one "missing".
  CHECK(ran.out.contains(R"("verified":3)"));
  CHECK(ran.err.contains("1 counterpart(s) missing during --verify-counterparts"));

  // No remediation flag: the link survives.
  CHECK(scalar(fx, "select count(*) from external_links") == 4);
  CHECK(scalar(fx, "select count(*) from sync_events where outcome = 'counterpart-missing'") == 1);
}

TEST_CASE("ext propagate --verify-counterparts --unlink deletes the missing link's row and exits clean",
          "[cmd][ext][propagate][verify][unlink]") {
  auto const fx = make_fixture("verify-unlink");
  {
    issue_counter                 seed_counter;
    planar::http::fixture::server seed_remote(make_respond_with_pull(seed_counter, {}));
    seed_single_repo(fx, seed_remote.base_url());
    REQUIRE(dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"}).code == 0);
  }
  auto const missing_external_id =
      text_scalar(fx, "select external_id from external_links where entity_kind = 'plan' and entity_id = 2");
  auto const hash = missing_external_id.rfind('#');
  REQUIRE(hash != std::string::npos);
  std::int64_t const missing_number = std::stoll(missing_external_id.substr(hash + 1));

  issue_counter                 counter;
  planar::http::fixture::server remote2(make_respond_with_pull(counter, {missing_number}));
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", remote2.base_url()))
                .has_value());
  }

  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--verify-counterparts", "--unlink", "--json"});
  CHECK(ran.code == 0);
  // `"ok"` is `failed==0 && missing==0`, unconditionally -- the oracle's own
  // definition (propagate.zig line 424) does not special-case `--unlink`/
  // `--recreate` remediation, so a clean-exit `--unlink` run still reports
  // `"ok":false` even though the exit CODE is 0 because the remediation
  // flag suppressed the die. Reproduced verbatim rather than "fixed".
  CHECK(ran.out.contains(R"("ok":false)"));
  CHECK(ran.out.contains(R"("missing":1)"));

  // The link row is gone; the sync_events audit trail for it survives with
  // link_id set to NULL (on delete set null), matching `record_mirror_link`'s
  // documented FK behavior.
  CHECK(scalar(fx, "select count(*) from external_links") == 3);
  CHECK(scalar(fx, "select count(*) from sync_events where outcome = 'counterpart-missing'") == 1);
}

// ---- --restrategize (task 6428) --------------------------------------------

TEST_CASE("ext propagate --restrategize --yes is a documented no-op once the only executable strategy is cached",
          "[cmd][ext][propagate][restrategize]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond_with_pull(counter, {}));
  auto const                    fx = make_fixture("restrategize");
  seed_single_repo(fx, remote.base_url());

  REQUIRE(dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--json"}).code == 0);
  REQUIRE(scalar(fx, "select count(*) from external_links") == 4);

  // Every entity is already linked, so this run's own tree-walk reports
  // skipped:4 regardless of --restrategize; the field under test is
  // "abandoned", which must stay absent (0) because the cached strategy
  // ("github-parent-issue", written by the propagate above) and the
  // freshly-selected strategy are the same string in this binary -- see
  // propagate.cppm's header on why that is the honest, not a shortcut,
  // outcome of --github-strategy/tracking-issue being unreachable here.
  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--restrategize", "--yes", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out.contains(R"("strategy":"github-parent-issue")"));
  CHECK(ran.out.contains(R"("skipped":4)"));
  CHECK_FALSE(ran.out.contains(R"("abandoned")"));
  CHECK(scalar(fx, "select count(*) from external_links") == 4);
  CHECK(scalar(fx, "select count(*) from sync_events where outcome = 'strategy-abandoned'") == 0);
}

// ---- --scope (task 6428) ---------------------------------------------------

TEST_CASE("ext propagate accepts --scope and discards it, matching the oracle's unguarded link-verb contract",
          "[cmd][ext][propagate][scope][unguarded]") {
  issue_counter                 counter;
  planar::http::fixture::server remote(make_respond(counter, 422));
  auto const                    fx = make_fixture("scope-discard");
  seed_single_repo(fx, remote.base_url());

  // "nonexistent-scope" resolves to nothing in this fixture -- if the flag
  // guarded anything, this run would refuse. It does not.
  auto const ran = dispatch(fx, {"ext", "propagate", "1", "--system", "gh-demo", "--scope", "nonexistent-scope", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out.contains(R"("created":4)"));
  CHECK(scalar(fx, "select count(*) from external_links") == 4);
}
