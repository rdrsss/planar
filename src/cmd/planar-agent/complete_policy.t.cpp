// complete_policy.t.cpp: `planar-agent complete` under the `review_required` and
// `task_gate` policy keys, driven the way an operator or an orchestrator drives it
// (plan 1142, task mo-policy-enforce-complete; tech spec 693 components 5-7; test
// spec 695, "Review and gate refusals at complete").
//
// Every case runs the BUILT binary through the harness's pinned environment, so
// the policy comes from the arena's `config.toml` (`PLANAR_CONFIG_PATH`) and the
// state from the arena's `planar.db`; nothing can reach the operator's
// `~/.planar`. The arena holds one plan, one task and one claim minted by the
// built `claim` verb under the session `claude:coder-1`, whose recorded
// repository root is the arena's working directory (`proj`). Gate evidence is
// seeded as `queue_history` rows in a build queue database of the arena's own
// (`PLANAR_BUILD_QUEUE_DB`), built with `apply_build_queue` and plain inserts, to
// reach the argv, outcome, tag and directory near misses a real run cannot produce
// cheaply. There is no queue binary to record a run yet.
//
// A refusal exits 1, names the unmet requirement and the command that would
// succeed, and writes nothing: the claim stays active and the task stays doing.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;

using parity::capture;
using parity::pinned_var;

/// @brief Run one statement on a database file, failing the test when it does not run.
/// @param path The database file; it must exist.
/// @param statement The SQL.
void exec_on(const std::filesystem::path& path, std::string_view statement) {
  auto conn = planar::db::connection::open_existing(path.string(), 5000);
  REQUIRE(conn.has_value());
  INFO(statement);
  REQUIRE(conn->execute(statement).has_value());
}

/// @brief Create a build queue database at `path` if it is not there yet. The connection closes before this
/// returns, so no `-wal` or `-shm` sidecar is left behind.
/// @param path The database file.
void ensure_queue_db(const std::filesystem::path& path) {
  if (std::filesystem::exists(path)) {
    return;
  }
  auto conn = planar::db::connection::open(path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_build_queue(*conn).has_value());
}

/// @brief Record one ended queue run in the build queue database at `path`, creating the database first.
/// @param path The database file.
/// @param seq The run's sequence number.
/// @param argv_json The argv as the JSON array text the queue engine writes.
/// @param outcome The `queue_history.outcome` text.
/// @param exit_code The exit status, or nullopt for none.
/// @param cwd The directory the run executed in.
/// @param tag The run's tag, or nullopt for an untagged run.
void record_run(const std::filesystem::path& path, std::int64_t seq, std::string_view argv_json, std::string_view outcome,
                std::optional<int> exit_code, std::string_view cwd, std::optional<std::string_view> tag) {
  ensure_queue_db(path);
  exec_on(path,
          std::format("insert into queue_history (seq, outcome, exit_code, cwd, argv, tag, enqueued_at, ended_at, waited_ms) "
                      "values ({}, '{}', {}, '{}', '{}', {}, 1759000000000, 1759000001000, 0)",
                      seq, outcome, exit_code.has_value() ? std::to_string(*exit_code) : std::string{"null"}, cwd, argv_json,
                      tag.has_value() ? std::format("'{}'", *tag) : std::string{"null"}));
}

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief One arena with a seeded plan and task, a minted claim, and helpers.
struct fixture {
  parity::arena arena;
  std::string   token;
  int           counter = 0;

  [[nodiscard]] auto db() const -> std::filesystem::path {
    return arena.cpp_root / "planar.db";
  }
  [[nodiscard]] auto proj() const -> std::filesystem::path {
    return arena.cpp_root / "proj";
  }
  /// @brief The build queue database every `agent` call resolves, unless a case names another.
  [[nodiscard]] auto queue_db() const -> std::filesystem::path {
    return arena.cpp_root / "build-queue.db";
  }

  auto agent(std::vector<std::string> args, std::vector<pinned_var> extra_env = {}) -> capture {
    auto env = parity::pinned_env(arena.cpp_root);
    env.push_back(pinned_var{.name = "PLANAR_BUILD_QUEUE_DB", .value = queue_db().string()});
    for (auto& var : extra_env) {
      // A case's variable replaces the pinned one of the same name, so a variable can be unset as well as moved.
      std::erase_if(env, [&](const pinned_var& have) { return have.name == var.name; });
      env.push_back(std::move(var));
    }
    return parity::run_pinned(agent_bin(), args, arena.cpp_root, std::format("run{}", counter++), env);
  }

  void sql(std::string_view statement) const {
    exec_on(db(), statement);
  }

  [[nodiscard]] auto scalar(std::string_view query) const -> std::string {
    auto conn = planar::db::connection::open_existing(db().string(), 5000);
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(query);
    REQUIRE(stmt.has_value());
    auto step = stmt->step();
    REQUIRE(step.has_value());
    REQUIRE(*step == planar::db::step_result::row);
    return stmt->column_text(0);
  }

  void write_config(std::string_view text) const {
    std::ofstream out(arena.cpp_root / "config.toml", std::ios::binary | std::ios::trunc);
    out << text;
  }

  /// @brief Nothing about the task or the claim changed: the proof that a refusal wrote nothing.
  [[nodiscard]] auto state() const -> std::string {
    return scalar("select (select status from tasks where id = 1) || '/' || (select status from agent_work_claims where id = 1) "
                  "|| '/' || (select count(*) from agent_actions) || '/' || (select count(*) from agent_actions "
                  "where ended_at is null)");
  }

  /// @brief Record a `queue_history` row in the arena's build queue database, as a tagged queue run would.
  void history(std::int64_t seq, std::string_view argv_json, std::string_view outcome, std::optional<int> exit_code,
               std::string_view cwd, std::optional<std::string_view> tag) const {
    record_run(queue_db(), seq, argv_json, outcome, exit_code, cwd, tag);
  }

  /// @brief Complete the claim.
  auto complete(std::vector<pinned_var> extra_env = {}) -> capture {
    return agent({"complete", "--claim", token, "--summary", "done"}, std::move(extra_env));
  }

  /// @brief Review the claim from another session with `verdict`.
  auto review(std::string_view verdict, std::string_view session = "claude:reviewer-1") -> capture {
    return agent({"review", "--claim", token, "--verdict", std::string{verdict}, "--summary", "looked", "--vendor-session",
                  std::string{session}});
  }
};

/// @brief Build the arena: migrated database, plan 1, task 1, a claim and its recorded locality.
auto make_fixture(std::string_view tag) -> fixture {
  fixture fx;
  fx.arena = parity::make_arena(tag);
  {
    auto conn = planar::db::connection::open(fx.db().string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global','p','policy-plan','active')")
                .has_value());
    REQUIRE(
        conn->execute("insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 't', 'todo', 100)")
            .has_value());
  }
  auto const got = fx.agent({"claim", "--entity", "task:1", "--vendor", "claude", "--vendor-session", "claude:coder-1", "--role",
                             "coder", "--no-locality-probe", "--ttl", "1h"});
  INFO("claim stdout:\n" << got.out << "\nstderr:\n" << got.err);
  REQUIRE(got.code == 0);
  constexpr std::string_view prefix = "claim:";
  REQUIRE(got.out.starts_with(prefix));
  auto const end = got.out.find_first_of(" \n", prefix.size());
  fx.token       = got.out.substr(prefix.size(), end - prefix.size());
  fx.sql(std::format("update agent_work_claims set repo_root = '{}' where claim_token = '{}'", fx.proj().string(), fx.token));
  return fx;
}

constexpr std::string_view k_ok = "ok task:1 status:done claim_status:completed\n";

} // namespace

TEST_CASE("review_required: complete refuses without a review, names the command, and writes nothing",
          "[cmd][agent][complete][policy][1142]") {
  auto fx = make_fixture("revreq");
  fx.write_config("[policy]\nreview_required = true\n");
  auto const before = fx.state();
  REQUIRE(before.starts_with("doing/active/"));

  auto const refused = fx.complete();
  CHECK(refused.code == 1);
  CHECK(refused.out.empty());
  CHECK(refused.err.starts_with(
      std::format("error: complete: claim {} cannot complete: review_required: ", fx.token.substr(0, 8))));
  CHECK(refused.err.contains(std::format("planar-agent review --claim '{}' --verdict approve --summary '<text>' "
                                         "--reviewer-claim '<other-token>'",
                                         fx.token)));
  CHECK(fx.state() == before);

  // The machine-readable form carries the same refusal under its own tag.
  auto const json = fx.agent({"complete", "--claim", fx.token, "--json"});
  CHECK(json.code == 1);
  CHECK(json.out.contains("PolicyUnmet"));
  CHECK(fx.state() == before);
}

TEST_CASE("review_required: a same-session, NULL-verdict or request-changes review does not satisfy it; an approval from another "
          "session does",
          "[cmd][agent][complete][policy][1142]") {
  auto fx = make_fixture("revkinds");
  fx.write_config("[policy]\nreview_required = true\n");

  // The review verb cannot write a self-review or a NULL verdict; seed them as a hostile writer would.
  fx.sql(std::format("insert into agent_actions (session_id, claim_id, action_kind, vendor, summary, verdict) "
                     "select session_id, id, 'review', 'claude', 'self', 'approve' from agent_work_claims "
                     "where claim_token = '{}'",
                     fx.token));
  auto const self = fx.complete();
  CHECK(self.code == 1);
  CHECK(self.err.contains("review_required: the latest approval"));
  CHECK(self.err.contains("claim's own session"));
  fx.sql("delete from agent_actions where action_kind = 'review'");

  fx.sql(std::format("insert into agent_actions (session_id, claim_id, action_kind, vendor, summary) "
                     "select session_id, id, 'review', 'claude', 'no verdict' from agent_work_claims where claim_token = '{}'",
                     fx.token));
  auto const unset = fx.complete();
  CHECK(unset.code == 1);
  CHECK(unset.err.contains("review_required: no review with a verdict is recorded"));
  fx.sql("delete from agent_actions where action_kind = 'review'");

  REQUIRE(fx.review("request-changes").code == 0);
  auto const changes = fx.complete();
  CHECK(changes.code == 1);
  CHECK(changes.err.contains("review_required: the latest review"));
  CHECK(changes.err.contains("requests changes"));
  CHECK(fx.scalar("select status from tasks where id = 1") == "doing");
  CHECK(fx.scalar("select status from agent_work_claims where id = 1") == "active");

  // A review from the claim's own session is refused by the verb itself, so it cannot unblock the claim.
  CHECK(fx.review("approve", "claude:coder-1").code != 0);
  CHECK(fx.complete().code == 1);

  REQUIRE(fx.review("approve", "claude:reviewer-2").code == 0);
  auto const done = fx.complete();
  CHECK(done.code == 0);
  CHECK(done.out == k_ok);
  CHECK(fx.state().starts_with("done/completed/"));
}

TEST_CASE("task_gate: only a passing run with the exact argv, this claim's token and this claim's directory satisfies it",
          "[cmd][agent][complete][policy][gate][1142]") {
  auto fx = make_fixture("gatekinds");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  auto const before = fx.state();
  auto const cwd    = fx.proj().string();
  auto const other  = std::string{"ffffffffffffffffffffffffffffffff"};

  auto const none = fx.complete();
  CHECK(none.code == 1);
  CHECK(none.err.starts_with(
      std::format("error: complete: claim {} cannot complete: task_gate: no passing queued run", fx.token.substr(0, 8))));
  CHECK(none.err.contains(std::format("planar-build-queue run --tag '{}' -- 'make' 'test'", fx.token)));
  CHECK_FALSE(none.err.contains("planar-agent queue run"));
  CHECK(fx.state() == before);
  CHECK_FALSE(std::filesystem::exists(fx.queue_db())); // the absent database is read, never created

  // Near misses, each one a row that would pass a looser matcher.
  std::int64_t seq = 5'000'001;
  fx.history(seq++, R"(["make","test","-j"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["make"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["sh","-c","make test"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["MAKE","TEST"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["make","test"])", "exited", 2, cwd, fx.token);              // failed
  fx.history(seq++, R"(["make","test"])", "signaled", std::nullopt, cwd, fx.token); // killed
  fx.history(seq++, R"(["make","test"])", "exited", 0, cwd, other);                 // another claim's run
  fx.history(seq++, R"(["make","test"])", "exited", 0, cwd, std::nullopt);          // untagged
  fx.history(seq++, R"(["make","test"])", "exited", 0, "/somewhere/else", fx.token);
  fx.history(seq++, R"(["make","test"])", "exited", 0, cwd + "/sub", fx.token); // a subdirectory
  auto const near = fx.complete();
  CHECK(near.code == 1);
  CHECK(near.err.contains("task_gate: no passing queued run"));
  CHECK(fx.state() == before);

  // The exact run in the claim's repository root satisfies the gate.
  fx.history(seq++, R"(["make","test"])", "exited", 0, cwd, fx.token);
  auto const done = fx.complete();
  CHECK(done.code == 0);
  CHECK(done.out == k_ok);
}

TEST_CASE("task_gate: a passing run tagged with the claim token, recorded in a quiet database with no sidecars, satisfies it",
          "[cmd][agent][complete][policy][gate]") {
  auto fx = make_fixture("gatequiet");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  fx.history(5'000'030, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token);
  auto const wal = std::filesystem::path{fx.queue_db().string() + "-wal"};
  auto const shm = std::filesystem::path{fx.queue_db().string() + "-shm"};
  REQUIRE(std::filesystem::exists(fx.queue_db()));
  REQUIRE_FALSE(std::filesystem::exists(wal));
  REQUIRE_FALSE(std::filesystem::exists(shm));
  auto const done = fx.complete();
  CHECK(done.code == 0);
  CHECK(done.out == k_ok);
  // The read-only open left no sidecar behind either.
  CHECK_FALSE(std::filesystem::exists(wal));
  CHECK_FALSE(std::filesystem::exists(shm));
}

TEST_CASE("task_gate: the build queue database is found through PLANAR_BUILD_QUEUE_DB", "[cmd][agent][complete][policy][gate]") {
  auto fx = make_fixture("gatepath");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  auto const elsewhere = fx.arena.cpp_root / "elsewhere" / "named.db";
  std::filesystem::create_directories(elsewhere.parent_path());
  record_run(elsewhere, 5'000'040, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token);
  auto const before = fx.state();

  // The default location has no database, so the variable is the only way to the passing run.
  REQUIRE_FALSE(std::filesystem::exists(fx.queue_db()));
  auto const refused = fx.complete();
  CHECK(refused.code == 1);
  CHECK(refused.err.contains("task_gate: no passing queued run"));
  CHECK(fx.state() == before);

  auto const done = fx.complete({pinned_var{.name = "PLANAR_BUILD_QUEUE_DB", .value = elsewhere.string()}});
  CHECK(done.code == 0);
  CHECK(done.out == k_ok);
}

TEST_CASE("task_gate: a variable that names a missing file in an empty directory is no evidence, and creates nothing",
          "[cmd][agent][complete][policy][gate]") {
  auto fx = make_fixture("gatemissing");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  fx.history(5'000'050, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token); // the default has the run
  auto const empty_dir = fx.arena.cpp_root / "empty";
  std::filesystem::create_directories(empty_dir);
  auto const before = fx.state();

  auto const refused = fx.complete({pinned_var{.name = "PLANAR_BUILD_QUEUE_DB", .value = (empty_dir / "none.db").string()}});
  CHECK(refused.code == 1);
  CHECK(refused.err.contains("task_gate: no passing queued run of the gate argv is recorded for this claim"));
  CHECK(refused.err.contains("planar-build-queue run --tag"));
  CHECK(fx.state() == before);
  CHECK(std::filesystem::is_empty(empty_dir));
}

TEST_CASE("task_gate: with neither PLANAR_BUILD_QUEUE_DB nor HOME the database cannot be located, which refuses as unreadable",
          "[cmd][agent][complete][policy][gate]") {
  auto fx = make_fixture("gatenoloc2");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  fx.history(5'000'055, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token);
  auto const before  = fx.state();
  auto const refused = fx.complete({pinned_var{.name = "PLANAR_BUILD_QUEUE_DB", .value = "", .unset = true},
                                    pinned_var{.name = "HOME", .value = "", .unset = true}});
  CHECK(refused.code == 1);
  CHECK(refused.err.contains("task_gate: the build queue database cannot be read: "));
  CHECK(refused.err.contains("PLANAR_BUILD_QUEUE_DB"));
  CHECK_FALSE(refused.err.contains("no passing queued run"));
  CHECK(fx.state() == before);
}

TEST_CASE("task_gate: a database that cannot be read refuses with the reason and never as missing evidence",
          "[cmd][agent][complete][policy][gate]") {
  auto fx = make_fixture("gateunreadable");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  auto const before = fx.state();
  auto const prefix = std::string{"task_gate: the build queue database cannot be read: "};

  SECTION("a file that is not a database") {
    std::ofstream(fx.queue_db(), std::ios::binary) << "this is not a sqlite database, just text padding the header out";
    auto const refused = fx.complete();
    CHECK(refused.code == 1);
    CHECK(refused.err.contains(prefix));
    CHECK(refused.err.contains(fx.queue_db().string()));
    CHECK_FALSE(refused.err.contains("no passing queued run"));
    CHECK(fx.state() == before);
  }
  SECTION("a database without the build queue identity") {
    {
      auto conn = planar::db::connection::open(fx.queue_db().string());
      REQUIRE(conn.has_value());
      REQUIRE(conn->execute("create table unrelated (x integer)").has_value());
    }
    auto const refused = fx.complete();
    CHECK(refused.code == 1);
    CHECK(refused.err.contains(prefix));
    CHECK_FALSE(refused.err.contains("no passing queued run"));
    CHECK(fx.state() == before);
  }
  SECTION("a database held by another connection past the busy bound") {
    fx.history(5'000'060, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token);
    // Rollback journal, so an exclusive lock blocks readers as well as writers.
    exec_on(fx.queue_db(), "pragma journal_mode = delete");
    auto holder = planar::db::connection::open_existing(fx.queue_db().string(), 1000);
    REQUIRE(holder.has_value());
    REQUIRE(holder->execute("begin exclusive").has_value());
    auto const refused = fx.complete();
    REQUIRE(holder->execute("rollback").has_value());
    CHECK(refused.code == 1);
    CHECK(refused.err.contains(prefix));
    CHECK_FALSE(refused.err.contains("no passing queued run"));
    CHECK(fx.state() == before);
    // Released, the same claim completes: the refusal wrote nothing.
    CHECK(fx.complete().code == 0);
  }
  SECTION("the refusal is tagged PolicyUnmet in JSON") {
    std::ofstream(fx.queue_db(), std::ios::binary) << "this is not a sqlite database, just text padding the header out";
    auto const json = fx.agent({"complete", "--claim", fx.token, "--json"});
    CHECK(json.code == 1);
    CHECK(json.out.contains("PolicyUnmet"));
    INFO("stdout:\n" << json.out << "\nstderr:\n" << json.err);
    CHECK((json.out + json.err).contains("the build queue database cannot be read"));
  }
}

TEST_CASE("task_gate without use_queue is a policy the file cannot apply, and complete refuses with that reason",
          "[cmd][agent][complete][policy][gate]") {
  auto       fx     = make_fixture("gatenoqueue");
  auto const before = fx.state();
  fx.history(5'000'070, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token);

  SECTION("a top-level gate with use_queue unset") {
    fx.write_config("[policy]\ntask_gate = [\"make\", \"test\"]\n");
  }
  SECTION("an association that inherits the gate and turns use_queue off") {
    fx.sql("insert into associations (id, slug, name, kind) values (1, 'acme', 'Acme', 'org')");
    fx.sql("update plans set scope_kind = 'association', scope_id = 1 where id = 1");
    fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n"
                    "[associations.\"acme\".policy]\nuse_queue = false\n");
  }
  auto const refused = fx.complete();
  CHECK(refused.code == 1);
  CHECK(refused.err.contains("the policy in config.toml cannot be applied"));
  CHECK(
      refused.err.contains("policy.task_gate: gate evidence is a queued run, so use_queue = true is required in the same scope"));
  CHECK(fx.state() == before);
}

TEST_CASE("a claim without a recorded location can show no gate run and the refusal says how to fix that",
          "[cmd][agent][complete][policy][gate][1142]") {
  auto fx = make_fixture("gatenoloc");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  fx.sql(std::format("update agent_work_claims set repo_root = null, worktree_path = null where claim_token = '{}'", fx.token));
  fx.history(5'000'010, R"(["make","test"])", "exited", 0, fx.proj().string(), fx.token);
  auto const refused = fx.complete();
  CHECK(refused.code == 1);
  CHECK(refused.err.contains("task_gate: the claim recorded no absolute repository root or worktree"));
  CHECK(refused.err.contains("without --no-locality-probe"));
}

TEST_CASE("a claim's worktree path is as good as its repository root", "[cmd][agent][complete][policy][gate][1142]") {
  auto fx = make_fixture("gatewt");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  auto const wt = (fx.arena.cpp_root / "wt").string();
  std::filesystem::create_directories(wt);
  fx.sql(std::format("update agent_work_claims set worktree_path = '{}' where claim_token = '{}'", wt, fx.token));
  fx.history(5'000'020, R"(["make","test"])", "exited", 0, wt, fx.token);
  CHECK(fx.complete().code == 0);
}

TEST_CASE("no policy: complete is unchanged, and the other terminal verbs are never held to the policy",
          "[cmd][agent][complete][policy][1142]") {
  SECTION("no config file at all") {
    auto fx = make_fixture("nopolicy");
    CHECK_FALSE(std::filesystem::exists(fx.arena.cpp_root / "config.toml"));
    auto const done = fx.complete();
    CHECK(done.code == 0);
    CHECK(done.out == k_ok);
  }
  SECTION("a config file whose policy keys do not touch complete") {
    auto fx = make_fixture("otherkeys");
    fx.write_config("[defaults]\nvendor = \"claude\"\n[policy]\nrequire_model = false\nuse_queue = true\n"
                    "confirm_before = [\"task done\"]\n");
    auto const done = fx.complete();
    CHECK(done.code == 0);
    CHECK(done.out == k_ok);
  }
  SECTION("review_required false is the default spelled out") {
    auto fx = make_fixture("explicitfalse");
    fx.write_config("[policy]\nreview_required = false\n");
    CHECK(fx.complete().code == 0);
  }
  SECTION("release, fail and block ignore the policy") {
    auto fx = make_fixture("otherverbs");
    fx.write_config("[policy]\nreview_required = true\n");
    auto const released = fx.agent({"release", "--claim", fx.token, "--reason", "not now"});
    CHECK(released.code == 0);
    CHECK(fx.scalar("select status from tasks where id = 1") == "todo");
  }
}

TEST_CASE("the environment cannot relax the policy", "[cmd][agent][complete][policy][1142]") {
  auto fx = make_fixture("envpolicy");
  fx.write_config("[policy]\nreview_required = true\n");
  auto const before = fx.state();

  for (std::string_view name :
       {"PLANAR_REVIEW_REQUIRED", "PLANAR_POLICY_REVIEW_REQUIRED", "PLANAR_POLICY_REVIEW", "REVIEW_REQUIRED"}) {
    INFO("variable " << name);
    auto const refused = fx.complete({pinned_var{.name = std::string{name}, .value = "false"}});
    CHECK(refused.code == 1);
    CHECK(refused.err.contains("review_required"));
  }
  CHECK(fx.state() == before);
}

TEST_CASE("a policy table in the claim's association applies to its tasks; the top-level table is the fallback",
          "[cmd][agent][complete][policy][1142]") {
  auto fx = make_fixture("assocpolicy");
  fx.sql("insert into associations (id, slug, name, kind) values (1, 'acme', 'Acme', 'org')");
  fx.sql("update plans set scope_kind = 'association', scope_id = 1 where id = 1");

  // The association asks for a review; the top level does not.
  fx.write_config("[policy]\nreview_required = false\n[associations.\"acme\".policy]\nreview_required = true\n");
  CHECK(fx.complete().code == 1);

  // Another association's table does not apply.
  fx.write_config("[policy]\nreview_required = false\n[associations.\"beta\".policy]\nreview_required = true\n");
  CHECK(fx.complete().code == 0);
}

TEST_CASE("a config whose policy cannot be applied refuses complete and says how to check it",
          "[cmd][agent][complete][policy][1142]") {
  auto       fx     = make_fixture("badconfig");
  auto const before = fx.state();

  fx.write_config("[policy]\nafter_review = \"dispatch\"\nreview_required = false\n");
  auto const unknown = fx.complete();
  CHECK(unknown.code == 1);
  CHECK(unknown.err.contains("cannot complete: the policy in config.toml cannot be applied"));
  CHECK(unknown.err.contains("policy.after_review"));
  CHECK(unknown.err.contains("planar config validate"));

  fx.write_config("[policy\nreview_required = true\n");
  auto const syntax = fx.complete();
  CHECK(syntax.code == 1);
  CHECK(syntax.err.contains("the policy in config.toml cannot be applied"));
  CHECK(fx.state() == before);

  // Fixing the file lets the same claim complete.
  fx.write_config("[policy]\nreview_required = false\n");
  CHECK(fx.complete().code == 0);
}
