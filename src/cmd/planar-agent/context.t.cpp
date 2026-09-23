// @file context.t.cpp
// @brief The `planar-agent` CONSUMER database policy, proven by what it
// refuses to do (plan 996, task 6107).
//
// The interesting assertions here are all NEGATIVE, because the policy is
// defined by absence: this binary does NOT migrate. A test that only
// checked "ensure_db returns a handle against a healthy database" would
// pass identically against the operator binary's migrating context and
// would therefore prove nothing about the boundary.
//
// HOME / DB SAFETY. Every case builds its own environment map over a unique
// scratch root and passes the database path to the context directly.
// Nothing here calls std::getenv, so there is no inherited-environment path
// by which the operator's ~/.planar could be reached.
//
// ## Break-probes run against this file
//
//   - Added `db::apply_all(*_db)` to `context::ensure_db` (i.e. made this
//     binary migrate like the operator one) -> `refuses a database it
//     would have to migrate` FAILS: the call now succeeds where it must
//     refuse. Restored -> green.
//   - Changed the `stored < maximum` refusal to fall through -> same test
//     FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.main;

namespace {

/// @brief A scratch root plus the environment map a case dispatches under.
struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_cmd_agent_ctx_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Construct a context over `fx`, writing to `out`/`err`.
/// @param fx The fixture.
/// @param out The stdout sink.
/// @param err The stderr sink.
/// @return The context.
auto make_context(const fixture& fx, std::ostream& out, std::ostream& err) -> planar::cmd::agent::context {
  return planar::cmd::agent::context{
      std::vector<std::string>{"planar-agent"}, planar::cmd::agent::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::agent::database>(fx.db_path, err), out, err};
}

/// @brief Create a fully-migrated database at `path`, the way `planar init`
/// would — this binary cannot do it itself, which is the point.
/// @param path Where to create it.
auto seed_migrated_db(const std::filesystem::path& path) -> void {
  auto opened = planar::db::connection::open(path.string());
  REQUIRE(opened.has_value());
  auto applied = planar::db::apply_all(*opened);
  REQUIRE(applied.has_value());
}

} // namespace

TEST_CASE("planar-agent context leaves stamp, filter, capsule, and resolve", "[cmd][agent][context]") {
  auto const fx = make_fixture("records");
  seed_migrated_db(fx.db_path);
  auto opened = planar::db::connection::open(fx.db_path.string());
  REQUIRE(opened);
  REQUIRE(opened->execute("insert into plans(scope_kind,title,slug,status) values('global','p','p-context','active');"));
  REQUIRE(opened->execute("insert into tasks(scope_kind,title,status) values('global','t','todo');"));
  REQUIRE(opened->execute("insert into sessions(vendor) values('test');"));
  REQUIRE(opened->execute(
      "insert into workflow_runs(plan_id,workflow_name,run_identifier,pid,repo_root) values(1,'wf','ctx-run',1,'/r');"));
  REQUIRE(opened->execute(
      "insert into "
      "agent_work_claims(claim_token,session_id,entity_kind,entity_id,claim_scope,status,vendor,lease_expires_at,run_id,stage) "
      "values('ctx-token',1,'task',1,'exclusive','active','test',strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 "
      "seconds'),1,'code');"));
  auto invoke = [&](std::vector<std::string> argv) {
    std::ostringstream          out, err;
    planar::cmd::agent::context ctx{
        std::move(argv), planar::cmd::agent::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::agent::database>(fx.db_path, err), out, err};
    auto root  = planar::cmd::agent::root_app();
    auto table = planar::cmd::agent::handlers(*root);
    auto code  = planar::cmd::agent::run(ctx, *root, table);
    return std::tuple{code, out.str(), err.str()};
  };
  auto [add_code, add_out, add_err] =
      invoke({"planar-agent", "context", "add", "--claim", "ctx-token", "--kind", "finding", "--body", "first", "--json"});
  CHECK(add_code == 0);
  CHECK(add_err.empty());
  CHECK(add_out.contains("\"run_id\":1"));
  CHECK(add_out.contains("\"stage\":\"code\""));
  auto [list_code, list_out, list_err] = invoke({"planar-agent", "context", "list", "--run", "1"});
  CHECK(list_code == 0);
  CHECK(list_err.empty());
  CHECK(list_out.contains("\"body\":\"first\""));
  auto [resolve_code, resolve_out, resolve_err] =
      invoke({"planar-agent", "context", "resolve", "--run", "1", "--stage", "code", "--status", "consumed", "--json"});
  CHECK(resolve_code == 0);
  CHECK(resolve_err.empty());
  CHECK(resolve_out == "{\"ok\":true,\"updated\":1,\"status\":\"consumed\"}\n");
  auto [capsule_code, capsule_out, capsule_err] =
      invoke({"planar-agent", "context", "capsule", "--run", "1", "--stage", "code", "--body", "summary", "--json"});
  CHECK(capsule_code == 0);
  CHECK(capsule_err.empty());
  CHECK(capsule_out.contains("\"claim_id\":null"));
}

TEST_CASE("planar-agent context keeps record lifecycle boundaries and filters observable", "[cmd][agent][context]") {
  auto const fx = make_fixture("record-boundaries");
  seed_migrated_db(fx.db_path);
  auto opened = planar::db::connection::open(fx.db_path.string());
  REQUIRE(opened);
  REQUIRE(
      opened->execute("insert into plans(scope_kind,title,slug,status) values('global','p','p-context-boundaries','active');"));
  REQUIRE(opened->execute("insert into tasks(scope_kind,title,status) values('global','t','todo');"));
  REQUIRE(opened->execute("insert into sessions(vendor) values('test');"));
  REQUIRE(opened->execute(
      "insert into workflow_runs(plan_id,workflow_name,run_identifier,pid,repo_root) values(1,'wf','unused-run',1,'/r');"));
  REQUIRE(opened->execute(
      "insert into workflow_runs(plan_id,workflow_name,run_identifier,pid,repo_root) values(1,'wf','ctx-boundaries',1,'/r');"));
  REQUIRE(opened->execute(
      "insert into "
      "agent_work_claims(claim_token,session_id,entity_kind,entity_id,claim_scope,status,vendor,lease_expires_at,run_id,stage) "
      "values('bound-token',1,'task',1,'exclusive','active','test',strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 "
      "seconds'),2,'plan');"));
  REQUIRE(opened->execute(
      "insert into "
      "agent_work_claims(claim_token,session_id,entity_kind,entity_id,claim_scope,status,vendor,lease_expires_at,stage) "
      "values('no-run-token',1,'task',1,'exclusive','active','test',strftime('%Y-%m-%dT%H:%M:%fZ','now','+600 "
      "seconds'),'plan');"));
  auto invoke = [&](std::vector<std::string> argv) {
    std::ostringstream          out, err;
    planar::cmd::agent::context ctx{
        std::move(argv), planar::cmd::agent::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::agent::database>(fx.db_path, err), out, err};
    auto root  = planar::cmd::agent::root_app();
    auto table = planar::cmd::agent::handlers(*root);
    auto code  = planar::cmd::agent::run(ctx, *root, table);
    return std::tuple{code, out.str(), err.str()};
  };

  auto rows = [&] {
    auto q = opened->prepare("select count(*) from context_records");
    REQUIRE(q);
    REQUIRE(q->step());
    return q->column_int64(0);
  };
  auto [bad_kind_code, bad_kind_out, bad_kind_err] =
      invoke({"planar-agent", "context", "add", "--claim", "bound-token", "--kind", "note", "--body", "ignored"});
  CHECK(bad_kind_code == 2);
  CHECK(bad_kind_out.empty());
  CHECK(bad_kind_err.contains("invalid --kind 'note'"));
  CHECK(rows() == 0);
  auto [no_run_code, no_run_out, no_run_err] =
      invoke({"planar-agent", "context", "add", "--claim", "no-run-token", "--kind", "finding", "--body", "ignored"});
  CHECK(no_run_code == 2);
  CHECK(no_run_out.empty());
  CHECK(no_run_err.contains("claim 'no-run-token' has no run_id"));
  CHECK(rows() == 0);

  auto [first_code, first_out, first_err] =
      invoke({"planar-agent", "context", "add", "--claim", "bound-token", "--kind", "finding", "--body", "plan-first", "--json"});
  CHECK(first_code == 0);
  CHECK(first_err.empty());
  CHECK(first_out.contains("\"stage\":\"plan\""));
  auto [second_code, second_out, second_err] = invoke({"planar-agent", "context", "add", "--claim", "bound-token", "--kind",
                                                       "risk", "--body", "plan-second", "--compiled-from", "4,5", "--json"});
  CHECK(second_code == 0);
  CHECK(second_err.empty());
  CHECK(second_out.contains("\"id\":2"));
  REQUIRE(opened->execute("update agent_work_claims set stage='code' where claim_token='bound-token';"));
  auto [third_code, third_out, third_err] = invoke(
      {"planar-agent", "context", "add", "--claim", "bound-token", "--kind", "artifact", "--body", "code-third", "--json"});
  CHECK(third_code == 0);
  CHECK(third_err.empty());
  CHECK(third_out.contains("\"stage\":\"code\""));

  auto [filtered_code, filtered_out, filtered_err] =
      invoke({"planar-agent", "context", "list", "--run", "2", "--stage", "plan", "--kind", "risk"});
  CHECK(filtered_code == 0);
  CHECK(filtered_err.empty());
  CHECK(filtered_out.contains("plan-second"));
  CHECK_FALSE(filtered_out.contains("plan-first"));
  CHECK_FALSE(filtered_out.contains("code-third"));
  auto [bulk_code, bulk_out, bulk_err] =
      invoke({"planar-agent", "context", "resolve", "--run", "2", "--stage", "plan", "--status", "consumed", "--json"});
  CHECK(bulk_code == 0);
  CHECK(bulk_err.empty());
  CHECK(bulk_out == "{\"ok\":true,\"updated\":2,\"status\":\"consumed\"}\n");
  auto [active_code, active_out, active_err] = invoke({"planar-agent", "context", "list", "--run", "2", "--status", "active"});
  CHECK(active_code == 0);
  CHECK(active_err.empty());
  CHECK(active_out.contains("code-third"));
  CHECK_FALSE(active_out.contains("plan-first"));
  auto [repeat_code, repeat_out, repeat_err] =
      invoke({"planar-agent", "context", "resolve", "--id", "1", "--status", "superseded"});
  CHECK(repeat_code == 2);
  CHECK(repeat_out.empty());
  CHECK(repeat_err.contains("record 1 is already in status 'consumed'"));

  auto [capsule_code, capsule_out, capsule_err] =
      invoke({"planar-agent", "context", "capsule", "--run", "2", "--stage", "plan", "--body", "capsule", "--session", "1",
              "--compiled-from", "1,2", "--json"});
  CHECK(capsule_code == 0);
  CHECK(capsule_err.empty());
  CHECK(capsule_out.contains("\"claim_id\":null"));
  auto capsule = opened->prepare("select claim_id,compiled_from,session_id from context_records where id=4");
  REQUIRE(capsule);
  REQUIRE(capsule->step());
  CHECK(capsule->is_null(0));
  CHECK(capsule->column_text(1) == "1,2");
  CHECK(capsule->column_int64(2) == 1);
}

TEST_CASE("planar-agent refuses a database it would have to migrate", "[cmd][agent][context]") {
  auto const         fx = make_fixture("behind");
  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, out, err);

  // A nonexistent path opens as a brand-new, EMPTY database — schema
  // version 0 — which is exactly the "fresh DB never touched by `planar
  // init`" case zig/src/runtime/runtime.zig calls out.
  auto const opened = ctx.db().ensure_db();
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::agent::domain_error_kind::schema_version_behind);

  // Exit 7, and this is the second per-binary divergence task 6066 found:
  // `planar` has NO SchemaVersionBehind arm and falls through to 1.
  CHECK(planar::cmd::agent::exit_code(opened.error()) == 7);

  // The remediation line reaches the operator, not just the error tag.
  CHECK(err.str().contains("run `planar init` to apply migrations"));
  CHECK(err.str().contains("is older than this binary's minimum of"));

  // The handle was released rather than left open in a refused state.
  CHECK_FALSE(ctx.db().opened());

  // AND THE DECISIVE PART: it did not migrate on the way out. The file it
  // opened is still at version 0.
  auto reopened = planar::db::connection::open(fx.db_path.string());
  REQUIRE(reopened.has_value());
  auto const version = planar::db::current_version(*reopened);
  CHECK((!version.has_value() || *version == 0));
}

TEST_CASE("planar-agent accepts a database already at the embedded version", "[cmd][agent][context]") {
  auto const fx = make_fixture("ok");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, out, err);

  auto const opened = ctx.db().ensure_db();
  REQUIRE(opened.has_value());
  CHECK(ctx.db().opened());
  CHECK(err.str().empty());

  // Read/write at the driver level — this binary DOES write
  // (agent_actions, agent_work_claims, ...). Contrast planar-watch, whose
  // context.t.cpp requires the symmetric write to FAIL.
  CHECK_FALSE((*opened)->is_read_only());
  auto const wrote = (*opened)->execute("create table cap_probe (id integer primary key)");
  CHECK(wrote.has_value());
}

TEST_CASE("planar-agent's ensure_db is idempotent and caches", "[cmd][agent][context]") {
  auto const fx = make_fixture("cached");
  seed_migrated_db(fx.db_path);

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx = make_context(fx, out, err);

  auto const first = ctx.db().ensure_db();
  REQUIRE(first.has_value());
  auto const second = ctx.db().ensure_db();
  REQUIRE(second.has_value());
  CHECK(*first == *second);
}

TEST_CASE("planar-agent resolves the database path PLANAR_DB-first, never PLANAR_HOME", "[cmd][agent][context]") {
  // CLAUDE.md calls mistaking PLANAR_HOME for PLANAR_DB "a common and
  // costly mistake"; the Zig original reads PLANAR_DB then HOME and
  // nothing else. A context that consulted PLANAR_HOME would find the
  // WRONG file on every real operator machine, where both are set.
  auto const explicit_db = planar::cmd::agent::resolve_db_path(planar::cmd::agent::map_env(
      {{"PLANAR_DB", "/tmp/explicit.db"}, {"PLANAR_HOME", "/tmp/some-home"}, {"HOME", "/tmp/real-home"}}));
  REQUIRE(explicit_db.has_value());
  CHECK(explicit_db->string() == "/tmp/explicit.db");

  auto const from_home = planar::cmd::agent::resolve_db_path(
      planar::cmd::agent::map_env({{"PLANAR_HOME", "/tmp/some-home"}, {"HOME", "/tmp/real-home"}}));
  REQUIRE(from_home.has_value());
  CHECK(from_home->string() == "/tmp/real-home/.planar/planar.db");

  auto const neither = planar::cmd::agent::resolve_db_path(planar::cmd::agent::map_env({}));
  REQUIRE_FALSE(neither.has_value());
}

TEST_CASE("planar-agent's operator cwd is PWD-first", "[cmd][agent][context]") {
  // Canonicalising resolves macOS's /var -> /private/var symlink, which
  // would break cwd-derived scope against a projects.root_path recorded
  // under /var (plan 351 task 2375).
  CHECK(planar::cmd::agent::operator_cwd(planar::cmd::agent::map_env({{"PWD", "/var/project"}})).string() == "/var/project");
  // An empty PWD falls through rather than yielding an empty path.
  CHECK_FALSE(planar::cmd::agent::operator_cwd(planar::cmd::agent::map_env({{"PWD", ""}})).empty());
}

TEST_CASE("planar-agent refuses a database migrated past its embedded chain", "[cmd][agent][context]") {
  // The SchemaVersionAhead arm (task 6690). Before this case only `planar`
  // had a UNIT test for it; the other three binaries pinned their `behind`
  // arm only, so task 6058's break-probe 2 -- disabling the ahead comparison
  // -- left this binary's suite GREEN. The process-level check covered it;
  // nothing at unit level did.
  //
  // Simulated the same way `planar`'s case does: migrate normally, then
  // insert a schema_migrations row above the embedded maximum, which is
  // exactly the state a newer binary leaves behind.
  auto const fx = make_fixture("ahead");
  seed_migrated_db(fx.db_path);
  {
    auto opened = planar::db::connection::open(fx.db_path.string());
    REQUIRE(opened.has_value());
    std::uint32_t embedded_max = 0;
    for (auto const& record : planar::db::migrations()) {
      embedded_max = std::max(embedded_max, record.version_);
    }
    auto const inserted = opened->execute(std::format(
        "insert into schema_migrations (version, description) values ({}, 'from a newer binary');", embedded_max + 1));
    REQUIRE(inserted.has_value());
  }

  std::ostringstream out;
  std::ostringstream err;
  auto               ctx    = make_context(fx, out, err);
  auto const         opened = ctx.db().ensure_db();
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().kind == planar::cmd::agent::domain_error_kind::schema_version_ahead);
  CHECK(planar::cmd::agent::exit_code(opened.error()) == 7);
}
