// @file terminal_diagnose.t.cpp
// @brief Workflow tests for the milestone diagnose trigger on the terminal verbs (plan 1132, task 7383).
//
// A milestone (a child plan) with two tasks is run through the real claim ritual against a scratch
// database. Pinned: `complete` on the task that promotes the milestone to `done` appends one newline
// and the diagnose section (text) or makes `diagnose` the last key of the single JSON object; a
// `complete` that promotes nothing prints exactly what it printed before; `fail`, `release` and
// `block` never print a section, even when they demote a `done` milestone; a diagnosis that cannot
// run prints `diagnose: unavailable (<reason>)` and changes neither the exit status nor the rows.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning;
import planar.engine.runtime.agentatomic;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.main;
import planar.cmd.planar_agent.policy;

namespace {

namespace agent = planar::cmd::agent;

struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_agent_terminal_diagnose_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] auto db_path() const -> std::filesystem::path {
    return path_ / "planar.db";
  }
};

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

/// Runs `argv` through the real tree and handler table against the scratch database. The config file
/// path points at an absent file, so `[introspection].cli_log` reads as its default (off).
auto run_verb(const scratch_dir& scratch, std::vector<std::string> argv) -> invocation {
  auto const root  = agent::root_app();
  auto const table = agent::handlers(*root);
  argv.insert(argv.begin(), "planar-agent");

  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx{argv,          agent::map_env({{"PLANAR_CONFIG_PATH", (scratch.path_ / "absent.toml").string()}}),
                         scratch.path_, std::make_shared<planar::cmd::agent::database>(scratch.db_path(), err),
                         out,           err};
  auto const         code = agent::run(ctx, *root, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto sql(const scratch_dir& scratch, std::string_view statement) -> void {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  auto done = conn->execute(statement);
  REQUIRE(done.has_value());
}

auto scalar_text(const scratch_dir& scratch, std::string_view query) -> std::string {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(query);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// Anchor plan 1, milestone plan 2 under it, and `tasks` todo tasks (ids 1..) in the milestone.
auto seed_milestone(const scratch_dir& scratch, int tasks) -> void {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global','anchor','anchor-plan','active')")
              .has_value());
  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                        "values ('global','milestone','milestone-plan','active',1)")
              .has_value());
  for (int i = 0; i < tasks; ++i) {
    REQUIRE(conn->execute(std::format("insert into tasks (scope_kind, plan_id, title, status, priority) "
                                      "values ('global', 2, 't{}', 'todo', {})",
                                      i, 100 + i))
                .has_value());
  }
}

/// Pulls the milestone's next task and returns the claim token.
auto pull_token(const scratch_dir& scratch) -> std::string {
  auto const pulled = run_verb(scratch, {"pull", "2", "--no-locality-probe"});
  REQUIRE(pulled.code == 0);
  return scalar_text(scratch, "select claim_token from agent_work_claims order by id desc limit 1");
}

constexpr std::string_view k_first_complete = "ok task:1 status:done claim_status:completed\n";

} // namespace

TEST_CASE("complete on the last task of a milestone prints the diagnose section after one newline",
          "[cmd][agent][diagnose][7383]") {
  scratch_dir scratch;
  seed_milestone(scratch, 2);

  auto const first = run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"});
  CHECK(first.code == 0);
  CHECK(first.out == k_first_complete);
  CHECK(first.err.empty());
  CHECK(scalar_text(scratch, "select status from plans where id = 2") == "active");

  auto const last = run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"});
  CHECK(last.code == 0);
  CHECK(last.out == "ok task:2 status:done claim_status:completed\n\ndiagnose: clean\n");
  CHECK(last.err.empty());
  CHECK(scalar_text(scratch, "select status from plans where id = 2") == "done");
}

TEST_CASE("complete --json makes diagnose the last key of the single object only on the promoting call",
          "[cmd][agent][diagnose][7383]") {
  scratch_dir scratch;
  seed_milestone(scratch, 2);

  auto const first = run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe", "--json"});
  REQUIRE(first.code == 0);
  CHECK(first.out.find("\"diagnose\"") == std::string::npos);
  CHECK(first.out.ends_with("}\n"));

  auto const last = run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe", "--json"});
  REQUIRE(last.code == 0);
  CHECK(last.out.find('\n') == last.out.size() - 1);
  auto const key = last.out.find(",\"diagnose\":{\"plan_id\":2,\"state\":\"clean\",\"outcome\":\"ok\"");
  REQUIRE(key != std::string::npos);
  // Nothing follows the diagnose object but the closing brace, and every earlier key is the plain envelope.
  CHECK(last.out.ends_with(",\"incidents\":{\"state\":\"not_applicable\",\"reason\":null}}}\n"));
  CHECK(last.out.starts_with("{\"ok\":true,\"claim_token\":\""));
  CHECK(last.out.find("\"task\":{", 0) < key);
  CHECK(last.out.find("\"diagnose\"", key + 12) == std::string::npos);
}

TEST_CASE("a finding on the milestone is printed and changes nothing about the complete", "[cmd][agent][diagnose][7383]") {
  scratch_dir scratch;
  seed_milestone(scratch, 2);

  REQUIRE(run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"}).code == 0);
  // The first task's claim is left `active` after the task finished: the 7337 shape.
  sql(scratch, "update agent_work_claims set status = 'active', released_at = null where id = 1");

  auto const last = run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"});
  CHECK(last.code == 0);
  CHECK(last.out.starts_with("ok task:2 status:done claim_status:completed\n\ndiagnose: 1 finding(s)\n"));
  CHECK(last.out.find("claim-superseded-active") != std::string::npos);
  CHECK(last.out.find("planar-agent abort --claim") != std::string::npos);
  CHECK(scalar_text(scratch, "select status from tasks where id = 2") == "done");
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 2") == "completed");
  CHECK(scalar_text(scratch, "select status from plans where id = 2") == "done");
}

TEST_CASE("complete on a task whose plan is an anchor, or that promotes nothing, prints no section",
          "[cmd][agent][diagnose][7383]") {
  scratch_dir scratch;
  seed_milestone(scratch, 1);
  // A task attached to the anchor: the anchor never auto-promotes to done.
  sql(scratch, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 'anchor-task', 'todo', 1)");
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  auto const token = scalar_text(scratch, "select claim_token from agent_work_claims order by id desc limit 1");
  auto const done  = run_verb(scratch, {"complete", "--claim", token, "--no-locality-probe", "--json"});
  CHECK(done.code == 0);
  CHECK(done.out.find("diagnose") == std::string::npos);
}

TEST_CASE("fail, release and block never print a section, even when they demote a done milestone",
          "[cmd][agent][diagnose][7383]") {
  struct verb_case {
    std::string              name;
    std::vector<std::string> extra;
  };
  std::vector<verb_case> const cases{
      {"fail", {"--reason", "broken"}},
      {"release", {"--reason", "stopping"}},
      {"block", {"--blocker", "1", "--reason", "waiting"}},
  };
  for (auto const& c : cases) {
    for (bool const json : {false, true}) {
      INFO(c.name << (json ? " --json" : ""));
      scratch_dir scratch;
      seed_milestone(scratch, 2);
      REQUIRE(run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"}).code == 0);
      REQUIRE(run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"}).code == 0);
      REQUIRE(scalar_text(scratch, "select status from plans where id = 2") == "done");

      // Reopen task 2 and claim it again, as `planar task reopen` followed by a claim would.
      sql(scratch, "update tasks set status = 'todo' where id = 2");
      auto const               token = pull_token(scratch);
      std::vector<std::string> argv{c.name, "--claim", token, "--no-locality-probe"};
      argv.insert(argv.end(), c.extra.begin(), c.extra.end());
      if (json) {
        argv.push_back("--json");
      }
      auto const result = run_verb(scratch, argv);
      CHECK(result.code == 0);
      CHECK(result.out.find("diagnose") == std::string::npos);
      CHECK(scalar_text(scratch, "select status from plans where id = 2") == "active");
    }
  }
}

TEST_CASE("a diagnosis that cannot run is reported without changing the complete", "[cmd][agent][diagnose][7383]") {
  scratch_dir scratch;
  seed_milestone(scratch, 1);
  // A table only the sync-conflict check reads: its query fails, the verb's own statements do not.
  sql(scratch, "drop table sync_events");

  auto const done = run_verb(scratch, {"complete", "--claim", pull_token(scratch), "--no-locality-probe"});
  CHECK(done.code == 0);
  CHECK(done.out == "ok task:1 status:done claim_status:completed\n\ndiagnose: unavailable (query-failed)\n");
  CHECK(done.err.empty());
  CHECK(scalar_text(scratch, "select status from plans where id = 2") == "done");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "done");
}

TEST_CASE("a milestone counts as promoted only by a fresh roll-up that flipped it to done", "[cmd][agent][diagnose][7383]") {
  namespace planning = planar::engine::planning;
  namespace atomic   = planar::engine::runtime::agentatomic;

  auto const roll_up = [](bool flipped, planning::plan_status after) {
    return agent::plan_roll_up{
        .result = planning::recompute_result{
            .plan_id = 2, .status_before = planning::plan_status::active, .status_after = after, .flipped = flipped}};
  };
  atomic::terminal_result fresh;
  atomic::terminal_result replay;
  replay.replayed = true;

  CHECK(agent::milestone_promoted(roll_up(true, planning::plan_status::done), fresh));
  // A replay wrote nothing, so a retry never reports the milestone twice.
  CHECK_FALSE(agent::milestone_promoted(roll_up(true, planning::plan_status::done), replay));
  // A roll-up that did not flip, or flipped to something else (a demotion), is not a promotion.
  CHECK_FALSE(agent::milestone_promoted(roll_up(false, planning::plan_status::done), fresh));
  CHECK_FALSE(agent::milestone_promoted(roll_up(true, planning::plan_status::active), fresh));
  // No roll-up ran (a plan-less task, or a verb that made none).
  CHECK_FALSE(agent::milestone_promoted(agent::plan_roll_up{}, fresh));
}
