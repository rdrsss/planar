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
// either seeded as `queue_history` rows (to reach argv, outcome, token and
// directory near misses a real run cannot produce cheaply) or recorded by a real
// `queue run --claim` of a stub command.
//
// A refusal exits 1, names the unmet requirement and the command that would
// succeed, and writes nothing: the claim stays active and the task stays doing.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {

namespace parity = planar::cmd::parity;
namespace qfix   = planar::cmd::qfix;

using parity::capture;
using parity::pinned_var;

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

  auto agent(std::vector<std::string> args, std::vector<pinned_var> extra_env = {}) -> capture {
    auto env = parity::pinned_env(arena.cpp_root);
    for (auto& var : extra_env) {
      env.push_back(std::move(var));
    }
    return parity::run_pinned(agent_bin(), args, arena.cpp_root, std::format("run{}", counter++), env);
  }

  void sql(std::string_view statement) const {
    qfix::exec(db(), statement);
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

  /// @brief Record a `queue_history` row as `queue run --claim` would have.
  void history(std::int64_t seq, std::string_view argv_json, std::string_view outcome, std::optional<int> exit_code,
               std::string_view cwd, std::string_view claim_token) const {
    sql(std::format(
        "insert into queue_history (seq, outcome, exit_code, cwd, argv, enqueued_at, ended_at, waited_ms, claim_token) "
        "values ({}, '{}', {}, '{}', '{}', 1759000000000, 1759000001000, 0, '{}')",
        seq, outcome, exit_code.has_value() ? std::to_string(*exit_code) : std::string{"null"}, cwd, argv_json, claim_token));
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
  CHECK(none.err.contains(std::format("planar-agent queue run --claim '{}' -- 'make' 'test'", fx.token)));
  CHECK(fx.state() == before);

  // Near misses, each one a row that would pass a looser matcher.
  std::int64_t seq = 5'000'001;
  fx.history(seq++, R"(["make","test","-j"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["make"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["sh","-c","make test"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["MAKE","TEST"])", "exited", 0, cwd, fx.token);
  fx.history(seq++, R"(["make","test"])", "exited", 2, cwd, fx.token);              // failed
  fx.history(seq++, R"(["make","test"])", "signaled", std::nullopt, cwd, fx.token); // killed
  fx.history(seq++, R"(["make","test"])", "exited", 0, cwd, other);                 // another claim's run
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

TEST_CASE("task_gate: a real queue run under the claim satisfies it, and the same command without --claim does not",
          "[cmd][agent][complete][policy][gate][1142]") {
  auto fx = make_fixture("gatereal");
  fx.write_config("[policy]\nuse_queue = true\ntask_gate = [\"sh\", \"-c\", \"exit 0\"]\n");

  // Same argv, no claim recorded: not evidence for this claim.
  auto const bare = fx.agent({"queue", "run", "--", "sh", "-c", "exit 0"});
  REQUIRE(bare.code == 0);
  CHECK(fx.complete().code == 1);

  // A failing run under the claim, then the passing one.
  auto const failing = fx.agent({"queue", "run", "--claim", fx.token, "--", "sh", "-c", "exit 3"});
  CHECK(failing.code == 3);
  CHECK(fx.complete().code == 1);

  auto const passing = fx.agent({"queue", "run", "--claim", fx.token, "--", "sh", "-c", "exit 0"});
  REQUIRE(passing.code == 0);
  auto const done = fx.complete();
  CHECK(done.code == 0);
  CHECK(done.out == k_ok);
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
