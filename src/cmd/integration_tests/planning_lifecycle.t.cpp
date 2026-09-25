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
  std::filesystem::path                  cwd;
  std::ostringstream                     diagnostics;
  std::shared_ptr<planar::cmd::database> database;

  fixture()
      : cwd(std::filesystem::temp_directory_path() /
            std::format("planar_integration_{}", std::chrono::steady_clock::now().time_since_epoch().count())),
        database(std::make_shared<planar::cmd::database>(":memory:", diagnostics)) {
    std::filesystem::create_directories(cwd);
  }

  ~fixture() {
    std::error_code ec;
    std::filesystem::remove_all(cwd, ec);
  }

  auto run(std::vector<std::string> args) -> invocation {
    std::vector<std::string> argv{"planar"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::ostringstream   out;
    std::ostringstream   err;
    planar::cmd::context ctx{
        std::move(argv), planar::cmd::map_env({{"HOME", cwd.string()}, {"PLANAR_HOME", cwd.string()}}), cwd, database, out, err};
    auto const tree  = planar::cmd::root_app();
    auto const table = planar::cmd::make_handler_table(*tree);
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
