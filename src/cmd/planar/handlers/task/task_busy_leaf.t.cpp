// @file task_busy_leaf.t.cpp
// @brief In-process black-box test for task 6843's residual: `planar task
// update` must report `Busy`/`busy_source` for a post-`busy_timeout`
// SQLITE_BUSY, never the generic `QueryFailed`/`generic_failure` bucket.
//
// Mirrors `annotate_leaves.t.cpp`'s "annotation command reports a competing
// write lock as retryable busy without a receipt" case: a second connection
// takes an `IMMEDIATE` lock (the RESERVED write lock, synchronously) and
// holds it open while the dispatched `task update` contends for the same
// row, using the real (uncontested-in-tests-elsewhere) default
// `busy_timeout=5000` connection::open sets (task 6842) — so this case
// genuinely waits out the retry window rather than shortcutting it, the
// same tradeoff annotate's busy case already makes.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.engine.planning;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int                                           code = 0; ///< The exit code.
  std::optional<planar::cmd::domain_error_kind> kind;     ///< The classified domain failure, when any.
  std::string                                   out;      ///< Everything written to stdout.
  std::string                                   err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment and database path this file's
/// cases dispatch against.
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
                         std::format("planar_taskbusy_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree    = planar::cmd::root_app();
  auto const         table   = planar::cmd::make_handler_table(*tree);
  auto const         outcome = planar::cmd::run_detailed(ctx, *tree, table);
  return invocation{.code = outcome.code, .kind = outcome.kind, .out = out.str(), .err = err.str()};
}

/// @brief Open the fixture's database directly, for row assertions and to
/// take the competing write lock.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

} // namespace

TEST_CASE("planar task update reports Busy / busy_source, not QueryFailed / generic_failure, "
          "under a competing write lock",
          "[cmd][task][busy][6843]") {
  auto const fx = make_fixture("update");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto       conn    = open_db(fx);
  auto const created = planar::engine::planning::create_task(
      conn, planar::engine::planning::task_create_args{.title = "Contended", .scope = "global"});
  REQUIRE(created.has_value());
  auto const id = std::to_string(created->id);

  auto locker = conn.begin_transaction(planar::db::lock_mode::immediate);
  REQUIRE(locker.has_value());

  auto const busy = dispatch(fx, {"task", "update", id, "--title", "New title", "--json"});

  CHECK(busy.code == 1);
  CHECK(busy.err == std::format("error: task update: Busy\n"));
  CHECK(busy.out == R"({"error":{"verb":"task update","tag":"busy_source"}}
)");
  REQUIRE(busy.kind.has_value());
  CHECK(*busy.kind == planar::cmd::domain_error_kind::busy_source);

  REQUIRE(locker->commit().has_value());

  // The contended write genuinely never landed while busy.
  auto const still = planar::engine::planning::show_task(conn, created->id);
  REQUIRE(still.has_value());
  CHECK(still->title == "Contended");

  // Once uncontended, the same command succeeds cleanly.
  auto const retried = dispatch(fx, {"task", "update", id, "--title", "New title", "--json"});
  CHECK(retried.code == 0);
  CHECK(retried.err.empty());
}

TEST_CASE("planar task done reports Busy / busy_source, not QueryFailed / generic_failure, "
          "under a competing write lock",
          "[cmd][task][busy][6907]") {
  // Task 6907: mark_done's (and mark_cancelled's / mark_blocked's /
  // reopen's) busy path is wired to `command_db_error` -- see
  // `engine/planning/task.cpp` -- exactly like `update_task`'s above, but
  // was never exercised by a contention test. This mirrors the `task
  // update` case above for `task done` to close that gap; `mark_done` is
  // reached identically (`begin_transaction`, `show_task`,
  // `check_transition`, `set_status`, `clear_unblocked_dependents`,
  // `record_audit`, `commit`), so the SAME competing IMMEDIATE lock forces
  // its commit to hit a post-`busy_timeout` SQLITE_BUSY.
  auto const fx = make_fixture("done");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto       conn    = open_db(fx);
  auto const created = planar::engine::planning::create_task(
      conn, planar::engine::planning::task_create_args{.title = "Contended", .scope = "global"});
  REQUIRE(created.has_value());
  auto const id = std::to_string(created->id);

  // `todo -> done` directly is not a legal transition (the task matrix
  // requires `doing` first); move it there BEFORE taking the competing
  // lock, so the contention below lands on `mark_done`'s own write, not
  // on this setup step.
  REQUIRE(dispatch(fx, {"task", "update", id, "--status", "doing"}).code == 0);

  auto locker = conn.begin_transaction(planar::db::lock_mode::immediate);
  REQUIRE(locker.has_value());

  auto const busy = dispatch(fx, {"task", "done", id, "--json"});

  CHECK(busy.code == 1);
  CHECK(busy.err == std::format("error: task done: Busy\n"));
  CHECK(busy.out == R"({"error":{"verb":"task done","tag":"busy_source"}}
)");
  REQUIRE(busy.kind.has_value());
  CHECK(*busy.kind == planar::cmd::domain_error_kind::busy_source);

  REQUIRE(locker->commit().has_value());

  // The contended write genuinely never landed while busy.
  auto const still = planar::engine::planning::show_task(conn, created->id);
  REQUIRE(still.has_value());
  CHECK(still->status == planar::engine::planning::task_status::doing);

  // Once uncontended, the same command succeeds cleanly.
  auto const retried = dispatch(fx, {"task", "done", id, "--json"});
  CHECK(retried.code == 0);
  CHECK(retried.err.empty());
}
