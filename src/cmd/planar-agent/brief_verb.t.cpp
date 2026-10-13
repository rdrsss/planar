// @file brief_verb.t.cpp
// @brief Tests for the `planar-agent brief` VERB (plan 1142, task mo-brief-verb): the real tree and
// handler table driven through `dispatch::run` against a scratch database and a scratch config file.
//
// The composition itself (`gather_brief`, the renderers) is covered by brief.t.cpp. What these cases
// pin is what the verb adds: the strictly read-only database open, the config file loading, the exit
// codes, the byte-exact JSON, and the catalog entry that pins the flags.
//
// Fixtures are seeded with SQL and the engine's `acquire_claim`, not by shelling `planar`: the test
// binary of a command target cannot reach a sibling command binary.
#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.json_dom;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.session;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.main;

namespace {

namespace agent = planar::cmd::agent;
namespace aa    = planar::engine::runtime::agentactivity;
namespace jd    = planar::json_dom;

struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_agent_brief_verb_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_ / "home");
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

  [[nodiscard]] auto config_path() const -> std::filesystem::path {
    return path_ / "config.toml";
  }

  auto write_config(std::string_view text) const -> void {
    std::ofstream out(config_path(), std::ios::binary | std::ios::trunc);
    out << text;
  }
};

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

/// Run `planar-agent <argv...>` through the real tree and handler table. `PLANAR_CONFIG_PATH` names the scratch
/// config file (absent until `write_config`), and `HOME` the scratch home, so nothing reads the operator's files.
auto run_verb(const scratch_dir& scratch, std::vector<std::string> argv) -> invocation {
  auto const root  = agent::root_app();
  auto const table = agent::handlers(*root);
  argv.insert(argv.begin(), "planar-agent");

  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx{
      argv,
      agent::map_env({{"PLANAR_CONFIG_PATH", scratch.config_path().string()}, {"HOME", (scratch.path_ / "home").string()}}),
      scratch.path_,
      std::make_shared<agent::database>(scratch.db_path(), err),
      out,
      err};
  auto const code = agent::run(ctx, *root, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto done = conn.execute(sql);
  INFO(sql);
  REQUIRE(done.has_value());
}

auto scalar(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// Open the scratch database read-write, creating and migrating it.
auto create_db(const scratch_dir& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

/// Anchor 1 with milestones 2 and 3; milestone 2 holds 20 todo tasks (1..20), a live claim on doing task 21, a stale claim on
/// doing task 22 and a done task 23; milestone 3 holds one todo task 24. The rule layout is brief.t.cpp's.
auto build_fixture(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id, slug, name) values (1, 'repo-one', 'repo-one')");
  exec(conn, "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'Anchor', 'p-anchor', 'active')");
  exec(conn, "insert into plans (id, scope_kind, title, slug, status, parent_plan_id) values "
             "(2, 'global', 'Milestone one', 'm1', 'active', 1), (3, 'global', 'Milestone two', 'm2', 'draft', 1)");
  for (int i = 1; i <= 20; ++i) {
    exec(conn, std::format("insert into tasks (id, scope_kind, plan_id, title, slug, status, priority) values "
                           "({0}, 'global', 2, 'Task {0}', 'task-{0}', 'todo', {1})",
                           i, 100 + i));
  }
  exec(conn,
       "insert into tasks (id, scope_kind, plan_id, title, slug, status, priority) values "
       "(21, 'global', 2, 'Held', 'held-live', 'doing', 200), (22, 'global', 2, 'Lapsed', 'held-stale', 'doing', 201), "
       "(23, 'global', 2, 'Finished', 'finished', 'done', 202), (24, 'global', 3, 'Next milestone', 'next-m2', 'todo', 300)");
  for (int i = 1; i <= 12; ++i) {
    exec(conn, std::format("insert into task_touch_paths (task_id, repo_id, path) values ({0}, 1, 'src/area{0}/file.cpp')", i));
  }
  exec(conn,
       "insert into task_touch_paths (task_id, repo_id, path) values "
       "(13, 1, 'src/area13/a.cpp'), (14, 1, 'src/area14/a.cpp'), (15, 1, 'src/area15/a.cpp'), (16, 1, 'src/area16/a.cpp'), "
       "(17, 1, 'src/shared/x.cpp'), (18, 1, 'src/shared/x.cpp'), (19, 1, 'migrations/00099_x.sql'), (20, 1, "
       "'docs/architecture.md')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 13, 'task', 14, "
             "'depends-on')");
  exec(conn, "insert into questions (scope_kind, title, status) values ('global', 'open q', 'open')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('task', 15, 'question', last_insert_rowid(), 'addresses')");
  exec(conn, "insert into decisions (scope_kind, title, body, status) values ('global', 'proposed d', 'body', 'proposed')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('task', 16, 'decision', last_insert_rowid(), 'addresses')");
  exec(conn, "insert into artifacts (scope_kind, kind, title, body, status) values ('global', 'tech_spec', 'old spec', 'b', "
             "'superseded')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('task', 12, 'artifact', last_insert_rowid(), 'cites')");

  auto const session = planar::engine::runtime::session::ensure_active(conn, "claude", std::nullopt);
  REQUIRE(session.has_value());
  REQUIRE(aa::acquire_claim(conn, aa::acquire_args{.session_id    = *session,
                                                   .kind          = aa::entity_kind::task,
                                                   .entity_id     = 21,
                                                   .vendor        = "claude",
                                                   .role          = "coder",
                                                   .model         = "claude-opus",
                                                   .worktree_path = std::string{"/tmp/it's a worktree"},
                                                   .ttl_secs      = 7200})
              .has_value());
  REQUIRE(aa::acquire_claim(conn, aa::acquire_args{.session_id = *session,
                                                   .kind       = aa::entity_kind::task,
                                                   .entity_id  = 22,
                                                   .vendor     = "claude",
                                                   .role       = "reviewer",
                                                   .ttl_secs   = -100})
              .has_value());
}

/// The golden fixture: milestone 2 of anchor 1 with two ready tasks on distinct paths, a live claim on a third and a stale
/// claim on a fourth, with fixed lease expiries so the bytes do not depend on the clock.
auto build_golden_fixture(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id, slug, name) values (1, 'repo-one', 'repo-one')");
  exec(conn, "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'Anchor', 'anc', 'active')");
  exec(conn, "insert into plans (id, scope_kind, title, slug, status, parent_plan_id) values "
             "(2, 'global', 'Milestone one', 'm1', 'active', 1)");
  exec(conn, "insert into tasks (id, scope_kind, plan_id, title, slug, status, priority) values "
             "(1, 'global', 2, 'Alpha', 'alpha', 'todo', 100), (2, 'global', 2, 'Beta', 'beta', 'todo', 101), "
             "(3, 'global', 2, 'Gamma', 'gamma', 'doing', 102), (4, 'global', 2, 'Delta', 'delta', 'doing', 103)");
  exec(conn, "insert into task_touch_paths (task_id, repo_id, path) values (1, 1, 'src/a.cpp'), (2, 1, 'src/b.cpp')");
  auto const session = planar::engine::runtime::session::ensure_active(conn, "claude", std::nullopt);
  REQUIRE(session.has_value());
  REQUIRE(aa::acquire_claim(conn, aa::acquire_args{.session_id    = *session,
                                                   .kind          = aa::entity_kind::task,
                                                   .entity_id     = 3,
                                                   .vendor        = "claude",
                                                   .role          = "coder",
                                                   .model         = "m",
                                                   .worktree_path = std::string{"/w/it's"},
                                                   .ttl_secs      = 7200})
              .has_value());
  REQUIRE(aa::acquire_claim(
              conn,
              aa::acquire_args{
                  .session_id = *session, .kind = aa::entity_kind::task, .entity_id = 4, .vendor = "claude", .ttl_secs = 7200})
              .has_value());
  exec(conn, "update agent_work_claims set lease_expires_at = '2099-01-01T00:00:00.000Z' where entity_id = 3");
  exec(conn, "update agent_work_claims set lease_expires_at = '2000-01-01T00:00:00.000Z' where entity_id = 4");
}

/// Every table row count the verb could conceivably touch.
auto row_totals(planar::db::connection& conn) -> std::vector<std::int64_t> {
  return {scalar(conn, "select count(*) from tasks"),
          scalar(conn, "select count(*) from agent_work_claims"),
          scalar(conn, "select count(*) from agent_actions"),
          scalar(conn, "select count(*) from sessions"),
          scalar(conn, "select count(*) from plans"),
          scalar(conn, "select count(*) from audit_log"),
          scalar(conn, "select count(*) from entity_links"),
          scalar(conn, "select count(*) from queue_entries"),
          scalar(conn, "select count(*) from schema_migrations")};
}

auto parse(const std::string& text) -> jd::json_value {
  auto parsed = jd::parse_json(text);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

auto ids_of(const jd::json_value* array) -> std::vector<std::int64_t> {
  REQUIRE(array != nullptr);
  std::vector<std::int64_t> out;
  for (auto const& item : array->array) {
    out.push_back(item.find("id")->integer);
  }
  return out;
}

} // namespace

TEST_CASE("brief of a 20-task milestone through the verb is under 4 KB and complete", "[cmd][agent][brief][verb]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }
  auto const r = run_verb(scratch, {"brief", "2"});
  INFO(r.out);
  INFO(r.err);
  REQUIRE(r.code == 0);
  CHECK(r.err.empty());
  CHECK(r.out.size() < 4096);
  CHECK(r.out.starts_with("brief v1 plan 2 m1 [active]\n"));
  CHECK(r.out.contains("tasks 23: todo 20 doing 2 blocked 0 done 1"));
  CHECK(r.out.contains("anchor: 1 p-anchor [active]"));
  CHECK(r.out.contains("live #"));
  CHECK(r.out.contains("stale #"));
  CHECK(r.out.contains("wave (12 parallel):"));
  CHECK(r.out.contains("13 task-13: rule 1: blocked_by not-done task 14"));
  CHECK(r.out.contains("12 task-12: packet not ready: unresolved_citation"));
  CHECK(r.out.contains("protocol (lease 7200s, heartbeat every 1h):"));
  CHECK(r.out.ends_with("\n"));
}

TEST_CASE("brief shell-quotes a worktree path with a single quote as '\\''", "[cmd][agent][brief][verb][quoting]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }
  auto const text = run_verb(scratch, {"brief", "2"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("worktree='/tmp/it'\\''s a worktree'"));
  CHECK_FALSE(text.out.contains("worktree=/tmp/it's"));

  // The JSON carries the raw path (JSON escaping, not shell quoting); the templates are the quoted part.
  auto const json = run_verb(scratch, {"brief", "2", "--json"});
  REQUIRE(json.code == 0);
  auto const parsed = parse(json.out);
  bool       saw    = false;
  for (auto const& claim : parsed.find("claims")->array) {
    if (claim.find("entity")->string == "task:21") {
      saw = true;
      CHECK(claim.find("worktree")->string == "/tmp/it's a worktree");
    }
  }
  CHECK(saw);
}

TEST_CASE("brief shows queue templates only when use_queue is true", "[cmd][agent][brief][verb][use_queue]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }

  SECTION("no config file: no queue template") {
    auto const r = run_verb(scratch, {"brief", "2"});
    REQUIRE(r.code == 0);
    CHECK_FALSE(r.out.contains("queue run"));
    CHECK(r.out.contains("use_queue=false [default]"));
  }
  SECTION("a gate without use_queue: still none, and the config reads as invalid") {
    scratch.write_config("[policy]\ntask_gate = [\"make\", \"test\"]\n");
    auto const r = run_verb(scratch, {"brief", "2"});
    REQUIRE(r.code == 0);
    CHECK_FALSE(r.out.contains("queue run"));
    CHECK(r.out.contains("task_gate=[\"make\",\"test\"] [policy]"));
    CHECK(r.out.contains("INVALID config: 1 finding(s)"));
  }
  SECTION("use_queue = false explicitly: none") {
    scratch.write_config("[policy]\nuse_queue = false\ntask_gate = [\"make\", \"test\"]\n");
    auto const r = run_verb(scratch, {"brief", "2"});
    REQUIRE(r.code == 0);
    CHECK_FALSE(r.out.contains("queue run"));
    CHECK(r.out.contains("INVALID config: 1 finding(s)"));
  }
  SECTION("use_queue = true shows the gate template, quoted, in text and JSON") {
    scratch.write_config("[policy]\nuse_queue = true\ntask_gate = [\"sh\", \"-c\", \"echo 'hi' $HOME\"]\n");
    auto const r = run_verb(scratch, {"brief", "2"});
    REQUIRE(r.code == 0);
    CHECK(r.out.contains("  gate: planar-agent queue run --claim '<token>' -- 'sh' '-c' 'echo '\\''hi'\\'' $HOME'\n"));
    CHECK(r.out.size() < 4096);

    auto const json = run_verb(scratch, {"brief", "2", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK(parsed.find("policy")->find("values")->find("use_queue")->boolean);
    bool saw = false;
    for (auto const& t : parsed.find("protocol")->find("templates")->array) {
      saw = saw || t.find("name")->string == "gate";
    }
    CHECK(saw);
  }
  SECTION("use_queue = true with no gate names a generic command") {
    scratch.write_config("[policy]\nuse_queue = true\n");
    auto const r = run_verb(scratch, {"brief", "2"});
    REQUIRE(r.code == 0);
    CHECK(r.out.contains("  gate: planar-agent queue run --claim '<token>' -- <command>\n"));
  }
}

TEST_CASE("brief reads the policy of the plan's association from the config file", "[cmd][agent][brief][verb][policy]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
    exec(conn, "insert into associations (id, slug, name, kind) values (1, 'acme', 'Acme', 'org')");
    exec(conn, "update plans set scope_kind = 'association', scope_id = 1 where id in (1, 2, 3)");
  }
  scratch.write_config("[policy]\nuse_queue = true\n[associations.\"acme\".policy]\nreview_required = true\n");
  auto const r = run_verb(scratch, {"brief", "2"});
  REQUIRE(r.code == 0);
  CHECK(r.out.contains("scope 'acme' (association_scope)"));
  CHECK(r.out.contains("review_required=true [associations.\"acme\".policy]"));
  CHECK(r.out.contains("use_queue=true [policy]"));
}

TEST_CASE("brief renders with policy.valid=false for an invalid or unreadable config and still exits 0",
          "[cmd][agent][brief][verb][config]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }

  SECTION("an unknown policy key") {
    scratch.write_config("[policy]\nafter_review = \"merge\"\nuse_queue = true\n");
    auto const text = run_verb(scratch, {"brief", "2"});
    REQUIRE(text.code == 0);
    CHECK(text.out.contains("INVALID config: 1 finding(s)"));
    // The valid key still applies: values fall through per key.
    CHECK(text.out.contains("use_queue=true [policy]"));

    auto const json = run_verb(scratch, {"brief", "2", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK_FALSE(parsed.find("policy")->find("valid")->boolean);
    REQUIRE(parsed.find("policy")->find("findings")->array.size() == 1);
    CHECK(parsed.find("policy")->find("findings")->array[0].find("key")->string == "policy.after_review");
  }
  SECTION("a task_gate without use_queue is a policy finding that makes the policy invalid") {
    scratch.write_config("[policy]\ntask_gate = [\"make\", \"test\"]\n");
    auto const json = run_verb(scratch, {"brief", "2", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK_FALSE(parsed.find("policy")->find("valid")->boolean);
    REQUIRE(parsed.find("policy")->find("findings")->array.size() == 1);
    auto const& finding = parsed.find("policy")->find("findings")->array[0];
    CHECK(finding.find("key")->string == "policy.task_gate");
    CHECK(
        finding.find("message")->string ==
        "gate evidence is a queued run, so use_queue = true is required in the same scope (add use_queue = true under [policy])");
  }
  SECTION("a mistyped value") {
    scratch.write_config("[policy]\nreview_required = \"yes\"\n");
    auto const json = run_verb(scratch, {"brief", "2", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK_FALSE(parsed.find("policy")->find("valid")->boolean);
    CHECK_FALSE(parsed.find("policy")->find("values")->find("review_required")->boolean);
  }
  SECTION("a config file that is not TOML") {
    scratch.write_config("[policy\nuse_queue = true\n");
    auto const text = run_verb(scratch, {"brief", "2"});
    REQUIRE(text.code == 0);
    CHECK(text.out.contains("INVALID config: 1 finding(s)"));
    CHECK(text.out.contains("use_queue=false [default]"));

    auto const json = run_verb(scratch, {"brief", "2", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK_FALSE(parsed.find("policy")->find("valid")->boolean);
    auto const& finding = parsed.find("policy")->find("findings")->array[0];
    CHECK(finding.find("key")->string == "config");
    CHECK(finding.find("message")->string.contains("TOML parse error"));
  }
  SECTION("a config path that is a directory") {
    std::filesystem::create_directories(scratch.config_path());
    auto const json = run_verb(scratch, {"brief", "2", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK_FALSE(parsed.find("policy")->find("valid")->boolean);
    CHECK(parsed.find("policy")->find("findings")->array[0].find("message")->string.contains("not a regular file"));
  }
}

TEST_CASE("brief writes nothing: row counts, file size and mtime are unchanged", "[cmd][agent][brief][verb][read-only]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }
  auto const                size_before  = std::filesystem::file_size(scratch.db_path());
  auto const                mtime_before = std::filesystem::last_write_time(scratch.db_path());
  std::vector<std::int64_t> totals_before;
  {
    auto reader = planar::db::connection::open_read_only(scratch.db_path().string());
    REQUIRE(reader.has_value());
    totals_before = row_totals(*reader);
  }

  scratch.write_config("[policy]\nuse_queue = true\n");
  for (auto const& argv : {std::vector<std::string>{"brief", "2"},
                           {"brief", "2", "--json"},
                           {"brief", "1"},
                           {"brief", "1", "--json"},
                           {"brief", "3"},
                           {"brief", "999"}}) {
    INFO("argv[1..]: " << argv.back());
    auto const r = run_verb(scratch, argv);
    CHECK((r.code == 0 || argv[1] == "999"));
  }

  CHECK(std::filesystem::file_size(scratch.db_path()) == size_before);
  CHECK((std::filesystem::last_write_time(scratch.db_path()) == mtime_before));
  auto reader = planar::db::connection::open_read_only(scratch.db_path().string());
  REQUIRE(reader.has_value());
  CHECK(row_totals(*reader) == totals_before);
  // And no sibling file was left behind by the verb beyond what SQLite keeps for a WAL reader.
  std::set<std::string> names;
  for (auto const& entry : std::filesystem::directory_iterator(scratch.path_)) {
    names.insert(entry.path().filename().string());
  }
  CHECK(names.contains("planar.db"));
  CHECK(names.contains("config.toml"));
  CHECK_FALSE(names.contains("planar.db-journal"));
}

TEST_CASE("brief opens the database strictly read-only, so a write fails whatever the code does",
          "[cmd][agent][brief][verb][read-only]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }
  std::ostringstream err;
  auto               conn = agent::database_policy::open_read_only(scratch.db_path(), err, 1000);
  REQUIRE(conn.has_value());
  CHECK(conn->is_read_only());
  CHECK_FALSE(conn->execute("update plans set title = 'x' where id = 1").has_value());
  CHECK_FALSE(conn->execute("insert into projects (slug, name) values ('x', 'x')").has_value());
  CHECK(err.str().empty());
}

TEST_CASE("brief of a plan that does not exist exits 1 and names the plan", "[cmd][agent][brief][verb][not-found]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }
  auto const r = run_verb(scratch, {"brief", "999"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err.contains("plan 999 not found"));

  auto const json = run_verb(scratch, {"brief", "999", "--json"});
  CHECK(json.code == 1);
  CHECK(json.err.contains("plan 999 not found"));
}

TEST_CASE("brief refuses a plan id that is not positive at exit 2", "[cmd][agent][brief][verb][args]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }
  auto const zero = run_verb(scratch, {"brief", "0"});
  CHECK(zero.code == 2);
  CHECK(zero.out.empty());
  CHECK(zero.err.contains("invalid plan id 0"));

  // A missing or non-numeric id is a usage error, which this binary reports at exit 1.
  CHECK(run_verb(scratch, {"brief"}).code == 1);
  CHECK(run_verb(scratch, {"brief", "abc"}).code == 1);
  CHECK(run_verb(scratch, {"brief", "2", "--no-such-flag"}).code == 1);
}

TEST_CASE("brief refuses a schema behind or ahead of the binary at exit 7 and a missing database at exit 1",
          "[cmd][agent][brief][verb][schema]") {
  scratch_dir scratch;

  SECTION("no database file") {
    auto const r = run_verb(scratch, {"brief", "1"});
    CHECK(r.code == 1);
    CHECK(r.err.contains("failed to open database"));
    CHECK_FALSE(std::filesystem::exists(scratch.db_path()));
  }
  SECTION("behind") {
    {
      auto conn = create_db(scratch);
      exec(conn, "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'P', 'p', 'active')");
      exec(conn, "delete from schema_migrations where version = (select max(version) from schema_migrations)");
    }
    auto const r = run_verb(scratch, {"brief", "1"});
    CHECK(r.code == 7);
    CHECK(r.out.empty());
    CHECK(r.err.contains("older than this binary"));
  }
  SECTION("ahead") {
    {
      auto conn = create_db(scratch);
      exec(conn, "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'P', 'p', 'active')");
      exec(conn, "insert into schema_migrations (version, description) values "
                 "((select max(version) + 1 from schema_migrations), 'from a newer build')");
    }
    auto const r = run_verb(scratch, {"brief", "1"});
    CHECK(r.code == 7);
    CHECK(r.out.empty());
    CHECK(r.err.contains("newer than this binary"));
  }
}

TEST_CASE("brief of a plan with no tasks is a complete empty brief", "[cmd][agent][brief][verb][empty]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    exec(conn, "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'Empty', 'empty', 'draft')");
  }
  auto const text = run_verb(scratch, {"brief", "1"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("tasks 0: todo 0 doing 0 blocked 0 done 0"));
  CHECK(text.out.contains("claims: none"));
  CHECK(text.out.contains("wave (0 parallel): none"));
  CHECK_FALSE(text.out.contains("serialized"));
  CHECK(text.out.contains("ready: 0 of 0 unclaimed todo"));
  CHECK_FALSE(text.out.contains("anchor:"));

  auto const json = run_verb(scratch, {"brief", "1", "--json"});
  REQUIRE(json.code == 0);
  auto const parsed = parse(json.out);
  CHECK(parsed.find("anchor")->kind == jd::json_kind::null_);
  CHECK(parsed.find("claims")->array.empty());
  CHECK(parsed.find("wave")->find("parallel")->array.empty());
  CHECK_FALSE(parsed.find("wave")->find("fan_out_available")->boolean);
  CHECK(parsed.find("readiness")->array.empty());
}

TEST_CASE("brief of a milestone names its anchor and its sibling milestones; of an anchor it aggregates every milestone",
          "[cmd][agent][brief][verb][anchor]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_fixture(conn);
  }

  SECTION("a milestone child plan counts only its own tree and names the anchor") {
    auto const json = run_verb(scratch, {"brief", "3", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK(parsed.find("plan")->find("slug")->string == "m2");
    CHECK(parsed.find("anchor")->find("slug")->string == "p-anchor");
    CHECK(parsed.find("counts")->find("todo")->integer == 1);
    CHECK(parsed.find("milestones")->array.size() == 2); // the anchor's children, siblings included
    CHECK(parsed.find("claims")->array.empty());         // the claims cover tasks of milestone 1
    CHECK(ids_of(parsed.find("readiness")) == std::vector<std::int64_t>{24});
  }
  SECTION("the anchor aggregates the tasks, claims and wave of all its milestones") {
    auto const json = run_verb(scratch, {"brief", "1", "--json"});
    REQUIRE(json.code == 0);
    auto const parsed = parse(json.out);
    CHECK(parsed.find("plan")->find("slug")->string == "p-anchor");
    CHECK(parsed.find("anchor")->kind == jd::json_kind::null_); // the plan is itself the anchor
    CHECK(parsed.find("counts")->find("todo")->integer == 21);  // 20 in milestone 1 and 1 in milestone 2
    CHECK(parsed.find("counts")->find("doing")->integer == 2);
    CHECK(parsed.find("counts")->find("done")->integer == 1);
    CHECK(parsed.find("milestones")->array.size() == 2);
    CHECK(parsed.find("claims")->array.size() == 2);
    CHECK(ids_of(parsed.find("wave")->find("parallel")) == std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14});
    // Task 24 declares no touch paths, so it is serialized (rule 2): the milestone it belongs to is in the aggregate.
    bool seen_24 = false;
    for (auto const id : ids_of(parsed.find("wave")->find("serialized"))) {
      seen_24 = seen_24 || id == 24;
    }
    CHECK(seen_24);
    CHECK(parsed.find("readiness")->array.size() == 21);
  }
}

TEST_CASE("brief --json is pinned byte for byte", "[cmd][agent][brief][verb][json][golden]") {
  scratch_dir scratch;
  {
    auto conn = create_db(scratch);
    build_golden_fixture(conn);
  }
  auto const r = run_verb(scratch, {"brief", "2", "--json"});
  REQUIRE(r.code == 0);
  CHECK(r.err.empty());

  // Keys in documented order, `brief_version` first. A change to this string is a change to the contract: bump
  // `brief_version` when a key is removed or changes meaning.
  constexpr std::string_view golden =
      R"GOLDEN({"brief_version":1,"plan":{"id":2,"slug":"m1","title":"Milestone one","status":"active"},"anchor":{"id":1,"slug":"anc","title":"Anchor","status":"active"},"counts":{"todo":2,"doing":2,"blocked":0,"done":0,"cancelled":0},"milestones":[{"plan":{"id":2,"slug":"m1","title":"Milestone one","status":"active"},"counts":{"todo":2,"doing":2,"blocked":0,"done":0,"cancelled":0}}],"claims":[{"claim_id":1,"entity":"task:3","state":"live","role":"coder","model":"m","worktree":"/w/it's","vendor":"claude","expires_at":"2099-01-01T00:00:00.000Z","covered_tasks":[3]},{"claim_id":2,"entity":"task:4","state":"stale","role":null,"model":null,"worktree":null,"vendor":"claude","expires_at":"2000-01-01T00:00:00.000Z","covered_tasks":[4]}],"wave":{"fan_out_available":true,"parallel":[{"id":1,"slug":"alpha","title":"Alpha","why":[]},{"id":2,"slug":"beta","title":"Beta","why":[]}],"serialized":[]},"readiness":[{"id":1,"slug":"alpha","ready":true,"reasons":[]},{"id":2,"slug":"beta","ready":true,"reasons":[]}],"policy":{"association":null,"scope_reason":"global_scope","valid":true,"findings":[],"digest":"3b63f1376435bab1a2ea9897beb537dd4158d86ae4934a2254d63e0ec31a6fa9","values":{"confirm_before":[],"require_model":false,"require_scenario_coverage":false,"review_required":false,"task_gate":null,"use_queue":false},"sources":{"review_required":{"kind":"default","section":""},"task_gate":{"kind":"default","section":""},"require_model":{"kind":"default","section":""},"confirm_before":{"kind":"default","section":""},"require_scenario_coverage":{"kind":"default","section":""},"use_queue":{"kind":"default","section":""}}},"confirm_before":[],"protocol":{"lease_ttl_secs":7200,"heartbeat_every_secs":3600,"templates":[{"name":"claim","command":"planar-agent claim --entity task:<id> --role coder --model '<model>' --ttl 7200 --worktree '<path>'"},{"name":"heartbeat","command":"planar-agent heartbeat --claim '<token>'"},{"name":"complete","command":"planar-agent complete --claim '<token>' --summary '<text>'"},{"name":"fail","command":"planar-agent fail --claim '<token>' --reason '<text>'"},{"name":"release","command":"planar-agent release --claim '<token>' --reason '<text>'"},{"name":"block","command":"planar-agent block --claim '<token>' --blocker <task-id> --reason '<text>'"},{"name":"review","command":"planar-agent review --claim '<reviewed-token>' --verdict approve|request-changes --summary '<text>'"}]}}
)GOLDEN";
  CHECK(r.out == golden);

  auto const               parsed = parse(r.out);
  std::vector<std::string> keys;
  for (auto const& [name, value] : parsed.object) {
    keys.push_back(name);
  }
  CHECK(keys == std::vector<std::string>{"brief_version", "plan", "anchor", "counts", "milestones", "claims", "wave", "readiness",
                                         "policy", "confirm_before", "protocol"});
}

TEST_CASE("the planar-agent schema catalog pins the brief verb's flags, positionals, examples and exit codes",
          "[cmd][agent][brief][verb][catalog]") {
  scratch_dir scratch;
  auto const  r = run_verb(scratch, {"schema"});
  REQUIRE(r.code == 0);
  auto const catalog = parse(r.out);

  jd::json_value const* brief = nullptr;
  for (auto const& command : catalog.find("commands")->array) {
    if (command.find("command")->string == "planar-agent brief") {
      brief = &command;
    }
  }
  REQUIRE(brief != nullptr);
  CHECK(brief->find("subcommands")->array.empty());

  REQUIRE(brief->find("flags")->array.size() == 1);
  auto const& flag = brief->find("flags")->array[0];
  CHECK(flag.find("long")->string == "--json");
  CHECK(flag.find("kind")->string == "bool");
  CHECK_FALSE(flag.find("required")->boolean);

  REQUIRE(brief->find("positionals")->array.size() == 1);
  auto const& positional = brief->find("positionals")->array[0];
  CHECK(positional.find("name")->string == "plan-id");
  CHECK(positional.find("kind")->string == "int");
  CHECK(positional.find("required")->boolean);

  auto const* docs = brief->find("docs");
  REQUIRE(docs != nullptr);
  std::vector<std::string> examples;
  for (auto const& e : docs->find("examples")->array) {
    examples.push_back(e.string);
  }
  CHECK(examples == std::vector<std::string>{"planar-agent brief 7", "planar-agent brief 7 --json"});
  std::vector<std::int64_t> codes;
  for (auto const& e : docs->find("exitCodes")->array) {
    codes.push_back(e.find("code")->integer);
  }
  CHECK(codes == std::vector<std::int64_t>{0, 1, 2, 7});
}
