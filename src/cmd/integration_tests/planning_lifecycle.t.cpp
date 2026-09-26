// @file planning_lifecycle.t.cpp
// @brief Multi-verb command scenarios over one injected in-memory SQLite DB.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

struct fixture {
  std::filesystem::path                  root;
  std::filesystem::path                  cwd;
  std::ostringstream                     diagnostics;
  std::shared_ptr<planar::cmd::database> database;

  fixture()
      : root(std::filesystem::temp_directory_path() /
             std::format("planar_integration_{}", std::chrono::steady_clock::now().time_since_epoch().count())),
        cwd(root / "proj"), database(std::make_shared<planar::cmd::database>(":memory:", diagnostics)) {
    std::filesystem::create_directories(cwd);
    std::filesystem::create_directories(root / "wb");
  }

  ~fixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  auto run(std::vector<std::string> args) -> invocation {
    std::vector<std::string> argv{"planar"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::ostringstream   out;
    std::ostringstream   err;
    planar::cmd::context ctx{std::move(argv),
                             planar::cmd::map_env({{"HOME", (root / "fakehome").string()},
                                                   {"PLANAR_HOME", (root / "home").string()},
                                                   {"PLANAR_WORKBENCH_ROOT", (root / "wb").string()},
                                                   {"PWD", cwd.string()}}),
                             cwd,
                             database,
                             out,
                             err};
    auto const           tree  = planar::cmd::root_app();
    auto const           table = planar::cmd::make_handler_table(*tree);
    return {.code = planar::cmd::run(ctx, *tree, table), .out = out.str(), .err = err.str()};
  }

  auto scalar_text(std::string_view sql) -> std::string {
    auto conn = database->ensure_db();
    REQUIRE(conn.has_value());
    auto stmt = (*conn)->prepare(sql);
    REQUIRE(stmt.has_value());
    auto step = stmt->step();
    REQUIRE(step.has_value());
    REQUIRE(*step == planar::db::step_result::row);
    return stmt->column_text(0);
  }

  auto scalar_int(std::string_view sql) -> std::int64_t {
    auto conn = database->ensure_db();
    REQUIRE(conn.has_value());
    auto stmt = (*conn)->prepare(sql);
    REQUIRE(stmt.has_value());
    auto step = stmt->step();
    REQUIRE(step.has_value());
    REQUIRE(*step == planar::db::step_result::row);
    return stmt->column_int64(0);
  }
};

} // namespace

TEST_CASE("plan command lifecycle rejects skipped and terminal transitions", "[integration][plan]") {
  fixture fx;
  REQUIRE(fx.database->path() == ":memory:");
  REQUIRE(fx.run({"plan", "create", "Lifecycle", "--slug", "lifecycle", "--scope", "global", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "draft");

  CHECK(fx.run({"plan", "update", "1", "--status", "done", "--json"}).code != 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "draft");

  REQUIRE(fx.run({"plan", "update", "1", "--status", "active", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "active");
  REQUIRE(fx.run({"plan", "update", "1", "--status", "paused", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "paused");
  REQUIRE(fx.run({"plan", "update", "1", "--status", "active", "--json"}).code == 0);
  REQUIRE(fx.run({"plan", "update", "1", "--status", "done", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "done");

  CHECK(fx.run({"plan", "update", "1", "--status", "active", "--json"}).code != 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "done");
}

TEST_CASE("task commands promote close and reopen a child plan", "[integration][task][plan]") {
  fixture fx;
  REQUIRE(fx.run({"plan", "create", "Anchor", "--slug", "anchor", "--scope", "global", "--json"}).code == 0);
  REQUIRE(fx.run({"plan", "create", "Child", "--slug", "child", "--parent", "1", "--scope", "global", "--json"}).code == 0);
  REQUIRE(fx.run({"task", "add", "Work", "--plan", "2", "--scope", "global", "--no-editor", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from plans where id = 2") == "draft");
  CHECK(fx.scalar_text("select status from tasks where id = 1") == "todo");

  CHECK(fx.run({"task", "done", "1", "--json"}).code != 0);
  CHECK(fx.scalar_text("select status from tasks where id = 1") == "todo");

  REQUIRE(fx.run({"task", "update", "1", "--status", "doing", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from plans where id = 2") == "active");
  REQUIRE(fx.run({"task", "done", "1", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from tasks where id = 1") == "done");
  CHECK(fx.scalar_text("select status from plans where id = 2") == "done");
  CHECK(fx.scalar_text("select status from plans where id = 1") == "draft");

  REQUIRE(fx.run({"task", "reopen", "1", "--status", "todo", "--reason", "regression", "--json"}).code == 0);
  CHECK(fx.scalar_text("select status from tasks where id = 1") == "todo");
  CHECK(fx.scalar_text("select status from plans where id = 2") == "active");
}

TEST_CASE("reviewed two-milestone plan ingests and advances through blocked work", "[integration][ingest][multi-plan]") {
  fixture    fx;
  auto const run_ok = [&](std::vector<std::string> args) -> invocation {
    auto result = fx.run(std::move(args));
    INFO(result.err << result.out);
    REQUIRE(result.code == 0);
    return result;
  };

  run_ok({"init", "--json"});
  run_ok({"assoc", "create", "project:proj", "--kind", "project", "--json"});
  run_ok({"assoc", "add", "project:proj", fx.cwd.string(), "--json"});
  run_ok({"plan", "create", "Ship a feature", "--slug", "ship-feature", "--json"});

  constexpr std::string_view product      = "## Intent\n\nShip a tested feature through two milestones.\n";
  constexpr std::string_view tech         = "## Decisions\n\n### Use a durable repository\n\nKeep state in SQLite.\n";
  constexpr std::string_view roadmap      = "## Foundation\n\n- Build schema [slug: build-schema] [touches: planar]\n"
                                            "- Implement repository [slug: implement-repository] [touches: planar]\n\n"
                                            "## Delivery\n\n- Add handler [slug: add-handler] [touches: planar]\n"
                                            "- Verify workflow [slug: verify-workflow] [touches: planar]\n";
  constexpr std::string_view tests        = "## Scenarios\n\n### Scenario: schema persists\n\n**Verifies:** task:build-schema\n\n"
                                            "### Scenario: repository reads\n\n**Verifies:** task:implement-repository\n\n"
                                            "### Scenario: handler dispatches\n\n**Verifies:** task:add-handler\n\n"
                                            "### Scenario: workflow completes\n\n**Verifies:** task:verify-workflow\n";
  auto const                 add_artifact = [&](std::string_view title, std::string_view kind, std::string_view body) {
    run_ok({"artifact", "add", std::string(title), "--kind", std::string(kind), "--plan", "1", "--body", std::string(body),
            "--json"});
  };
  add_artifact("Product", "product_spec", product);
  add_artifact("Tech", "tech_spec", tech);
  add_artifact("Roadmap", "roadmap", roadmap);
  add_artifact("Tests", "test_spec", tests);
  for (int id = 1; id <= 4; ++id) {
    run_ok({"artifact", "update", std::to_string(id), "--status", "active", "--json"});
  }
  run_ok({"workbench", "push", "1", "--json"});

  auto preview = run_ok({"spec", "ingest", "1", "--strict", "--json"});
  CHECK(preview.out.contains("\"uncovered_task_slugs\": []"));
  CHECK(preview.out.contains("\"orphan_scenarios\": []"));
  CHECK(fx.scalar_int("select count(*) from plans") == 1);
  CHECK(fx.scalar_int("select count(*) from tasks") == 0);
  run_ok({"spec", "ingest", "1", "--strict", "--apply", "--json"});
  CHECK(fx.scalar_int("select count(*) from plans where parent_plan_id = 1") == 2);
  CHECK(fx.scalar_int("select count(*) from tasks") == 4);
  CHECK(fx.scalar_int("select count(*) from test_scenarios") == 4);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "active");
  run_ok({"decision", "accept", "1", "--json"});
  run_ok({"spec", "ingest", "1", "--apply", "--json"});
  CHECK(fx.scalar_int("select count(*) from tasks") == 4);
  auto const task_id = [&](std::string_view slug) {
    return std::to_string(fx.scalar_int(std::format("select id from tasks where slug = '{}'", slug)));
  };

  auto const post_ingest = run_ok({"test-spec", "status", "1", "--json"});
  CHECK(post_ingest.out.contains("Foundation"));
  CHECK(post_ingest.out.contains("Delivery"));
  for (auto const slug : {"build-schema", "implement-repository", "add-handler", "verify-workflow"}) {
    auto const packet = run_ok({"task", "packet", task_id(slug), "--json"});
    INFO(slug << ": " << packet.out.substr(packet.out.size() > 500 ? packet.out.size() - 500 : 0));
    CHECK(packet.out.contains("\"ready\":true"));
  }
  auto const next = run_ok({"plan", "next", "1", "--json"});
  CHECK(next.out.contains(R"("available":[{"id":1,)"));
  CHECK(next.out.contains("Build schema"));

  run_ok({"task", "add", "Preflight", "--plan", "1", "--slug", "preflight", "--no-editor", "--json"});
  CHECK(fx.scalar_int("select count(*) from tasks where plan_id = 1") == 1);
  CHECK(fx.scalar_int("select count(*) from tasks where plan_id = 2") == 2);
  CHECK(fx.scalar_int("select count(*) from tasks where plan_id = 3") == 2);
  auto const schema     = task_id("build-schema");
  auto const repository = task_id("implement-repository");
  auto const handler    = task_id("add-handler");
  auto const verify     = task_id("verify-workflow");
  auto const preflight  = task_id("preflight");

  auto const blocked_closeout = run_ok({"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(blocked_closeout.out.contains("\"ready\":false"));
  run_ok({"task", "block", repository, "--on", schema, "--reason", "schema is required", "--json"});
  run_ok({"task", "block", handler, "--on", repository, "--reason", "repository is required", "--json"});
  run_ok({"task", "block", verify, "--on", handler, "--reason", "handler is required", "--json"});
  CHECK(fx.scalar_int("select count(*) from tasks where status = 'blocked'") == 3);

  run_ok({"task", "update", preflight, "--status", "doing", "--json"});
  run_ok({"task", "done", preflight, "--json"});
  CHECK(fx.scalar_text("select status from plans where id = 1") == "active");
  run_ok({"task", "update", schema, "--status", "doing", "--json"});
  run_ok({"task", "done", schema, "--json"});
  CHECK(fx.scalar_text(std::format("select status from tasks where id = {}", repository)) == "todo");
  run_ok({"task", "update", repository, "--status", "doing", "--json"});
  run_ok({"task", "done", repository, "--json"});
  CHECK(fx.scalar_text("select status from plans where id = 2") == "done");
  CHECK(fx.scalar_text(std::format("select status from tasks where id = {}", handler)) == "todo");

  run_ok({"task", "update", handler, "--status", "doing", "--json"});
  run_ok({"task", "done", handler, "--json"});
  CHECK(fx.scalar_text(std::format("select status from tasks where id = {}", verify)) == "todo");
  run_ok({"task", "update", verify, "--status", "doing", "--json"});
  run_ok({"task", "done", verify, "--json"});
  CHECK(fx.scalar_text("select status from plans where id = 3") == "done");
  CHECK(fx.scalar_int("select count(*) from tasks where status = 'done'") == 5);
  CHECK(fx.scalar_int("select count(*) from tasks where status = 'blocked'") == 0);
  CHECK(fx.scalar_text("select status from plans where id = 1") == "active");

  auto const ready_closeout = run_ok({"plan", "closeout", "1", "--dry-run", "--json"});
  CHECK(ready_closeout.out.contains("\"ready\":true"));
  run_ok({"plan", "closeout", "1", "--json"});
  CHECK(fx.scalar_text("select status from plans where id = 1") == "done");
}
