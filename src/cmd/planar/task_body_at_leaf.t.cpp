// @file task_body_at_leaf.t.cpp
// @brief In-process black-box tests for task 6848: `task add --body` and
// `task update --body` must read the `@path` grammar (decision D10),
// reusing the SAME refusal shape `artifact update --body` already has
// (`error: read --body: FileNotFound` at exit 2 — note the message does
// NOT name the path; that asymmetry with `--from-file` is the oracle's own
// and is pinned in `artifact_leaves.t.cpp`, reproduced here rather than
// improved on).
//
// Before this task, both verbs stored the literal `@path` token verbatim,
// silently — no refusal, exit 0, a body column that looks like a stray
// shell glob rather than a file's contents.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
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

/// @brief A scratch root plus the environment and database path this
/// file's cases dispatch against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory. Nothing here
/// reads the real environment, so there is no path by which the operator's
/// `~/.planar/planar.db` can be reached.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_taskbody_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Open the fixture's database directly, for row assertions.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief Read a single task's body column, or unset when the row does not
/// exist.
/// @param conn An open connection.
/// @param id The task's row id.
/// @return The stored body, `<NULL>` rendered as unset.
auto task_body(planar::db::connection& conn, std::int64_t id) -> std::optional<std::string> {
  auto stmt = conn.prepare("select body from tasks where id = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  if (*step != planar::db::step_result::row) {
    return std::nullopt;
  }
  return std::string{stmt->column_text(0)};
}

} // namespace

TEST_CASE("task update --body @missing refuses at exit 2 and leaves the body unchanged", "[cmd][task][body][6848]") {
  auto const fx = make_fixture("updatemissing");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T", "--scope", "global", "--body", "original", "--json"}).code == 0);

  auto const refused = dispatch(fx, {"task", "update", "1", "--body", "@nope-6848.md", "--json"});
  CHECK(refused.code == 2);
  CHECK(refused.err == "error: read --body: FileNotFound\n");

  auto conn = open_db(fx);
  CHECK(task_body(conn, 1) == "original");
}

TEST_CASE("task add --body @missing refuses at exit 2 and creates no row", "[cmd][task][body][6848]") {
  auto const fx = make_fixture("addmissing");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto const refused = dispatch(fx, {"task", "add", "T", "--scope", "global", "--body", "@nope-6848.md", "--json"});
  CHECK(refused.code == 2);
  CHECK(refused.err == "error: read --body: FileNotFound\n");

  auto conn       = open_db(fx);
  auto count_stmt = conn.prepare("select count(*) from tasks");
  REQUIRE(count_stmt.has_value());
  REQUIRE(count_stmt->step().has_value());
  CHECK(count_stmt->column_int64(0) == 0);
}

TEST_CASE("task add/update --body @realfile reads the file's bytes, not the literal token", "[cmd][task][body][6848]") {
  auto const fx = make_fixture("realfile");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto const path = fx.root / "body-6848.md";
  {
    std::ofstream f(path, std::ios::binary);
    f << "file contents, with an @ sign in it too";
  }
  auto const abs = path.string();

  REQUIRE(dispatch(fx, {"task", "add", "T", "--scope", "global", "--body", "@" + abs, "--json"}).code == 0);
  auto       conn         = open_db(fx);
  auto const created_body = task_body(conn, 1);
  REQUIRE(created_body.has_value());
  CHECK(*created_body == "file contents, with an @ sign in it too");
  CHECK(!created_body->starts_with('@'));

  REQUIRE(dispatch(fx, {"task", "add", "T2", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "2", "--body", "@" + abs, "--json"}).code == 0);
  auto const updated_body = task_body(conn, 2);
  REQUIRE(updated_body.has_value());
  CHECK(*updated_body == "file contents, with an @ sign in it too");
  CHECK(!updated_body->starts_with('@'));
}

TEST_CASE("a body whose literal text merely CONTAINS an @ later is stored verbatim, unaffected", "[cmd][task][body][6848]") {
  auto const fx = make_fixture("literalat");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "add", "T", "--scope", "global", "--body", "reach me at foo@example.com", "--json"}).code == 0);
  auto conn = open_db(fx);
  CHECK(task_body(conn, 1) == "reach me at foo@example.com");

  REQUIRE(dispatch(fx, {"task", "add", "T2", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "2", "--body", "bar@example.org is another one", "--json"}).code == 0);
  CHECK(task_body(conn, 2) == "bar@example.org is another one");
}
