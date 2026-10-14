// health_conformance.t.cpp: `planar health conformance <plan-id>` driven the way an operator
// drives it (plan 1142, task mo-conformance-health; test spec 695, "health reports conformance
// for a plan").
//
// Plans and tasks are made with the real verbs. Claims, which only `planar-agent` writes, are
// raw rows in the scratch database. The rule logic itself is pinned by the engine module's own
// tests; these cases pin what the verb adds: the exit code, the entity ids in the output, the
// `--json` payload, the `--scope` filter, the `--floor` override, the policy file it reads, that
// it writes nothing, and its refusals.
//
// Every fixture roots PLANAR_DB, PLANAR_HOME and HOME inside its own scratch directory, and
// nothing here reads the process environment.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "parity_harness.hpp"

namespace {

using planar::cmd::context;

/// One handler invocation's observable result.
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

auto make_fixture(std::string_view tag, std::optional<std::string_view> config = std::nullopt) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_hc_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "fakehome", ec);
  fixture fx{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
  if (config.has_value()) {
    auto const path = root / "config.toml";
    std::ofstream(path) << *config;
    fx.vars["PLANAR_CONFIG_PATH"] = path.string();
  }
  return fx;
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());
  auto const         cwd = fx.root / "proj";
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{
      std::move(argv), planar::cmd::map_env(fx.vars), cwd, std::make_shared<planar::cmd::database>(fx.db_path, err), out, err};
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::make_handler_table(*tree);
  int const  code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// Runs the real `planar-agent` in the fixture's pinned scratch environment (the fixture's database and `config.toml`).
auto agent(const fixture& fx, std::vector<std::string> args) -> invocation {
  static int counter = 0;
  auto const got     = planar::cmd::parity::run_pinned(std::filesystem::path{PLANAR_AGENT_CPP_BIN}, args, fx.root,
                                                       std::format("agent{}", ++counter));
  return invocation{.code = got.code, .out = got.out, .err = got.err};
}

/// The claim token in a `pull --json` reply.
auto claim_token(const invocation& pulled) -> std::string {
  std::string const key = "\"claim_token\":\"";
  auto const        at  = pulled.out.find(key);
  REQUIRE(at != std::string::npos);
  auto const begin = at + key.size();
  return pulled.out.substr(begin, pulled.out.find('"', begin) - begin);
}

/// Runs raw SQL against the fixture's database (opened after a verb has migrated it).
void sql(const fixture& fx, std::string_view statement) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  INFO(statement);
  REQUIRE(conn->execute(statement).has_value());
}

auto count(const fixture& fx, std::string_view query) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(query);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// A claim row on `task_id` the way `planar-agent` would leave it (the verbs that write claims are another binary's).
void add_claim(const fixture& fx, int task_id, std::string_view status, std::string_view role = "coder",
               std::string_view model = "'m'") {
  sql(fx, "insert into sessions (vendor) values ('test')");
  auto const token = std::format("{:032x}", count(fx, "select count(*) from agent_work_claims") + 1);
  sql(fx,
      std::format("insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, role, model, "
                  "lease_expires_at, released_at) values ('{}', (select max(id) from sessions), 'task', {}, '{}', 'test', "
                  "'{}', {}, '2999-01-01T00:00:00.000Z', {})",
                  token, task_id, status, role, model, status == "active" ? "null" : "'2026-02-02T00:00:00.000Z'"));
  sql(fx, std::format("insert into agent_actions (session_id, claim_id, parent_action_id, action_kind, vendor) "
                      "values ((select max(id) from sessions), (select max(id) from agent_work_claims), "
                      "(select min(id) from agent_actions), 'coder', 'test')"));
}

/// Two plans in global scope: plan 1 with an unclaimed done task, a forced done task and a stale claim
/// (so it breaks rules 1 and 2 and lists one override), and plan 2 with one properly completed task.
/// Plan 1's tasks are 1 (plain done, no claim), 2 (forced done), 3 (claimed, then stale).
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--name", "proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Violating", "--scope", "global", "--slug", "violating"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Clean", "--scope", "global", "--slug", "clean"}).code == 0);
  for (auto const& title : {"unclaimed", "forced", "stale-claimed"}) {
    REQUIRE(dispatch(fx, {"task", "add", title, "--plan", "1", "--scope", "global"}).code == 0);
  }
  REQUIRE(dispatch(fx, {"task", "add", "properly done", "--plan", "2", "--scope", "global"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "2", "--force"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "3", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "4", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "4"}).code == 0);

  // The claim ids are 1 (stale, on task 3) and 2 (completed, on task 4). A first action is the parent both hang under.
  sql(fx, "insert into sessions (vendor) values ('test')");
  sql(fx, "insert into agent_actions (session_id, action_kind, vendor) values (1, 'orchestrator', 'test')");
  add_claim(fx, 3, "stale");
  add_claim(fx, 4, "completed");
}

} // namespace

TEST_CASE("health conformance lists each violation with its entity id, the override and the floor, and exits non-zero",
          "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("violating");
  seed(fx);

  auto const run = dispatch(fx, {"health", "conformance", "1"});
  CHECK(run.code == 1);
  CHECK(run.out.contains("conformance: plan 1 (1 plan graded)"));
  CHECK(run.out.contains("violations: 2"));
  CHECK(run.out.contains("rule 1 completed-claim: task 1:"));
  CHECK(run.out.contains("rule 2 terminal-verb: claim 1 ["));
  CHECK(run.out.contains("on task:3"));
  // The forced done is listed apart from the violations, and is not one.
  CHECK(run.out.contains("overrides: 1\n  task 2: operator moved the task to done with --force"));
  CHECK_FALSE(run.out.contains("rule 1 completed-claim: task 2"));
  CHECK(run.out.contains("before the floor: 0 claims, 0 done tasks"));
  CHECK(run.err.contains("plan 1 does not conform: 2 violations"));
}

TEST_CASE("health conformance exits zero on a clean trail", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("clean");
  seed(fx);

  auto const run = dispatch(fx, {"health", "conformance", "2"});
  CHECK(run.code == 0);
  CHECK(run.err.empty());
  CHECK(run.out.contains("violations: none"));
  CHECK(run.out.contains("overrides: none"));
  CHECK(run.out.contains("graded: 1 claims, 1 done tasks"));
}

TEST_CASE("health conformance --json carries the same facts as the text", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("json");
  seed(fx);

  auto const bad = dispatch(fx, {"health", "conformance", "1", "--json"});
  CHECK(bad.code == 1);
  CHECK(bad.out.starts_with("{\"plan_id\":1,"));
  CHECK(bad.out.contains("\"conforms\":false"));
  CHECK(bad.out.contains("\"rule\":1,\"name\":\"completed-claim\",\"entity_kind\":\"task\",\"entity_id\":1,"));
  CHECK(bad.out.contains("\"rule\":2,\"name\":\"terminal-verb\",\"entity_kind\":\"claim\",\"entity_id\":1,"));
  CHECK(bad.out.contains("\"overrides\":[{\"task_id\":2,"));
  CHECK(bad.out.ends_with("}\n"));

  auto const good = dispatch(fx, {"health", "conformance", "2", "--json"});
  CHECK(good.code == 0);
  CHECK(good.out.contains("\"conforms\":true"));
  CHECK(good.out.contains("\"violations\":[]"));
}

TEST_CASE("health conformance --floor replaces the default cut-over and counts the rest before the floor",
          "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("floor");
  seed(fx);

  // Claim 1 is the stale one; a floor of 2 puts it before the floor, so it no longer fails.
  auto const run = dispatch(fx, {"health", "conformance", "1", "--floor", "2"});
  CHECK(run.out.contains("floor: claim 2 (explicit"));
  CHECK_FALSE(run.out.contains("terminal-verb"));
  CHECK(run.out.contains("before the floor: 1 claims, "));

  CHECK(dispatch(fx, {"health", "conformance", "1", "--floor", "0"}).code == 2);
  CHECK(dispatch(fx, {"health", "conformance", "1", "--floor", "-3"}).code == 2);
}

TEST_CASE("health conformance never fails history from before the floor", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("history");
  seed(fx);
  // Migration 00042 was applied after everything in plan 1 happened.
  sql(fx, "update schema_migrations set applied_at = '2999-01-01T00:00:00.000Z' where version = 42");

  auto const run = dispatch(fx, {"health", "conformance", "1"});
  CHECK(run.code == 0);
  CHECK(run.out.contains("violations: none"));
  CHECK(run.out.contains("before the floor: 1 claims, 2 done tasks"));
}

TEST_CASE("health conformance reads the policy of the plan's scope from config.toml", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("policy", "[policy]\nrequire_model = true\n");
  seed(fx);
  // Task 4's claim records no model.
  sql(fx, "update agent_work_claims set model = null where id = 2");

  auto const run = dispatch(fx, {"health", "conformance", "2"});
  CHECK(run.code == 1);
  CHECK(run.out.contains("rule 3 coder-model: claim 2 ["));

  // Without the policy key the same trail conforms.
  auto const lax = make_fixture("nopolicy");
  seed(lax);
  sql(lax, "update agent_work_claims set model = null where id = 2");
  CHECK(dispatch(lax, {"health", "conformance", "2"}).code == 0);
}

TEST_CASE("health conformance refuses a config.toml whose policy cannot be applied", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("badpolicy", "[policy]\nafter_review = \"merge\"\n");
  seed(fx);

  auto const run = dispatch(fx, {"health", "conformance", "2"});
  CHECK(run.code == 1);
  CHECK(run.err.contains("the policy in config.toml cannot be applied"));
  CHECK(run.out.empty());
}

TEST_CASE("health conformance --scope is a read filter on the plan's scope", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("scope");
  seed(fx);

  CHECK(dispatch(fx, {"health", "conformance", "2", "--scope", "global"}).code == 0);
  auto const other = dispatch(fx, {"health", "conformance", "2", "--scope", "assoc:elsewhere"});
  CHECK(other.code == 1);
  CHECK(other.err.contains("plan 2 is in scope 'global', not 'assoc:elsewhere'"));
  CHECK(other.out.empty());
}

TEST_CASE("health conformance refuses an unknown plan and a malformed id", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("refuse");
  seed(fx);

  auto const missing = dispatch(fx, {"health", "conformance", "99"});
  CHECK(missing.code == 1);
  CHECK(missing.err.contains("no plan with id 99"));
  auto const malformed = dispatch(fx, {"health", "conformance", "abc"});
  CHECK(malformed.code == 2);
  CHECK(malformed.err.contains("plan id must be an integer, got 'abc'"));
}

TEST_CASE("health conformance writes nothing", "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("readonly");
  seed(fx);
  auto const audit_before  = count(fx, "select count(*) from audit_log");
  auto const claims_before = count(fx, "select count(*) from agent_work_claims");
  auto const tasks_before  = count(fx, "select count(*) from tasks where status = 'done'");
  auto const stamp_before  = count(fx, "select sum(unixepoch(updated_at)) from tasks");

  dispatch(fx, {"health", "conformance", "1"});
  dispatch(fx, {"health", "conformance", "2", "--json"});

  CHECK(count(fx, "select count(*) from audit_log") == audit_before);
  CHECK(count(fx, "select count(*) from agent_work_claims") == claims_before);
  CHECK(count(fx, "select count(*) from tasks where status = 'done'") == tasks_before);
  CHECK(count(fx, "select sum(unixepoch(updated_at)) from tasks") == stamp_before);
}

TEST_CASE("the force marker `task done --force` leaves is what the checker reads as an override",
          "[cmd][planar][health][conformance]") {
  auto const fx = make_fixture("marker");
  seed(fx);
  CHECK(count(fx, "select count(*) from audit_log where entity_kind = 'task' and entity_id = 2 and summary = 'done (--force)'") ==
        1);
  CHECK(count(fx, "select count(*) from audit_log where entity_kind = 'task' and entity_id = 1 and summary = 'done (--force)'") ==
        0);
}

TEST_CASE("a claim taken and completed with the real planar-agent is graded under the policy it ran under",
          "[cmd][planar][health][conformance][wiring]") {
  auto const fx = make_fixture("real", "[policy]\nrequire_model = true\nreview_required = true\n");
  REQUIRE(dispatch(fx, {"init", "--name", "proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "P", "--scope", "global", "--slug", "p", "--status", "active"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T", "--plan", "1", "--scope", "global"}).code == 0);

  // The policy is enforced where the claim is taken and completed...
  CHECK(agent(fx, {"pull", "1", "--role", "coder", "--vendor", "claude", "--vendor-session", "claude:none"}).code != 0);
  auto const pulled = agent(
      fx, {"pull", "1", "--role", "coder", "--model", "m", "--vendor", "claude", "--vendor-session", "claude:coder", "--json"});
  REQUIRE(pulled.code == 0);
  auto const token = claim_token(pulled);
  CHECK(agent(fx, {"complete", "--claim", token}).code != 0);
  REQUIRE(agent(fx, {"review", "--claim", token, "--verdict", "approve", "--summary", "ok", "--vendor", "claude",
                     "--vendor-session", "claude:reviewer"})
              .code == 0);
  REQUIRE(agent(fx, {"complete", "--claim", token}).code == 0);

  // ...and the checker reads the digest that claim recorded as the current policy, so it grades rules 3 and 6.
  auto const clean = dispatch(fx, {"health", "conformance", "1", "--floor", "1", "--json"});
  CHECK(clean.code == 0);
  CHECK(clean.out.contains("\"policy_changed\":[]"));
  CHECK(clean.out.contains("\"violations\":[]"));
  CHECK(count(fx, "select count(*) from agent_work_claims where policy_digest is not null") == 1);

  sql(fx, "update agent_work_claims set model = null");
  sql(fx, "delete from agent_actions where action_kind = 'review'");
  auto const broken = dispatch(fx, {"health", "conformance", "1", "--floor", "1", "--json"});
  CHECK(broken.code == 1);
  CHECK(broken.out.contains("\"policy_changed\":[]"));
  CHECK(broken.out.contains("\"rule\":3,\"name\":\"coder-model\""));
  CHECK(broken.out.contains("\"rule\":6,\"name\":\"independent-review\""));
}

TEST_CASE("the checker resolves a claim's policy scope as the claim did, for a task in a descendant plan",
          "[cmd][planar][health][conformance][wiring]") {
  // The association table turns require_model on; the top-level table leaves it off. A checker that resolved the
  // wrong scope would record a different digest (policy_changed) or miss the missing model.
  auto const fx = make_fixture("assoc", "[policy]\nrequire_model = false\n[associations.\"project:proj\".policy]\n"
                                        "require_model = true\n");
  REQUIRE(dispatch(fx, {"init", "--name", "proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--status", "active"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Child", "--parent", "1", "--status", "active"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T", "--plan", "2"}).code == 0);

  // The association's require_model is what refuses a model-less coder pull, so this scope is the claim's.
  CHECK(agent(fx, {"pull", "2", "--role", "coder", "--vendor", "claude", "--vendor-session", "claude:none"}).code != 0);
  auto const pulled = agent(
      fx, {"pull", "2", "--role", "coder", "--model", "m", "--vendor", "claude", "--vendor-session", "claude:coder", "--json"});
  REQUIRE(pulled.code == 0);
  REQUIRE(agent(fx, {"complete", "--claim", claim_token(pulled)}).code == 0);

  auto const clean = dispatch(fx, {"health", "conformance", "1", "--floor", "1", "--json"});
  CHECK(clean.code == 0);
  CHECK(clean.out.contains("\"plans\":2"));
  CHECK(clean.out.contains("\"policy_changed\":[]"));

  sql(fx, "update agent_work_claims set model = null");
  auto const broken = dispatch(fx, {"health", "conformance", "1", "--floor", "1", "--json"});
  CHECK(broken.code == 1);
  CHECK(broken.out.contains("\"policy_changed\":[]"));
  CHECK(broken.out.contains("\"rule\":3,\"name\":\"coder-model\""));
}
