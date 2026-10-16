// claim_policy.t.cpp: `planar-agent claim` and `pull` under the `require_model` policy key, and the
// policy digest every claim records (plan 1142, task mo-policy-claim-and-ingest; tech spec 693
// component 6; test spec 695, "Claim-time policy").
//
// Every case runs the BUILT binary through the harness's pinned environment, so the policy comes from
// the arena's `config.toml` (`PLANAR_CONFIG_PATH`) and the state from the arena's `planar.db`; nothing
// can reach the operator's `~/.planar`. The arena holds two plans and three tasks, none claimed:
// plan 1 (tasks 1 and 3) and plan 2 (task 2), plus one plan step on plan 1. A refusal exits 1 with the
// `PolicyUnmet` tag, names the flag that would satisfy it, and writes nothing: no claim row, no action
// row, and the task stays `todo`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.config;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {

namespace parity = planar::cmd::parity;
namespace qfix   = planar::cmd::qfix;
namespace cfg    = planar::engine::config;

using parity::capture;
using parity::pinned_var;

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

constexpr std::string_view k_require_model_text =
    "claim: policy require_model = true: coder claims must say which model they use (add --model '<id>')";

struct fixture {
  parity::arena arena;
  int           counter = 0;

  [[nodiscard]] auto db() const -> std::filesystem::path {
    return arena.cpp_root / "planar.db";
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

  /// @brief Claim `entity` with the given role and model; unset arguments are left off the command line.
  auto claim(std::string_view entity, std::optional<std::string_view> role, std::optional<std::string_view> model,
             std::vector<pinned_var> extra_env = {}) -> capture {
    std::vector<std::string> args{"claim", "--entity", std::string{entity}, "--vendor", "claude", "--no-locality-probe"};
    if (role.has_value()) {
      args.insert(args.end(), {"--role", std::string{*role}});
    }
    if (model.has_value()) {
      args.insert(args.end(), {"--model", std::string{*model}});
    }
    return agent(std::move(args), std::move(extra_env));
  }

  auto pull(std::int64_t plan, std::optional<std::string_view> role, std::optional<std::string_view> model) -> capture {
    std::vector<std::string> args{"pull", std::to_string(plan), "--vendor", "claude", "--no-locality-probe"};
    if (role.has_value()) {
      args.insert(args.end(), {"--role", std::string{*role}});
    }
    if (model.has_value()) {
      args.insert(args.end(), {"--model", std::string{*model}});
    }
    return agent(std::move(args));
  }

  /// @brief The digest the newest claim recorded, or `NULL`.
  [[nodiscard]] auto newest_digest() const -> std::string {
    return scalar("select coalesce(policy_digest, 'NULL') from agent_work_claims order by id desc limit 1");
  }

  /// @brief Nothing was written: the proof that a refusal left the arena as it found it.
  [[nodiscard]] auto state() const -> std::string {
    return scalar("select (select count(*) from agent_work_claims) || '/' || (select count(*) from agent_actions) || '/' || "
                  "(select group_concat(status) from (select status from tasks order by id))");
  }
};

auto make_fixture(std::string_view tag) -> fixture {
  fixture fx;
  fx.arena  = parity::make_arena(tag);
  auto conn = planar::db::connection::open(fx.db().string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  for (auto const* statement : {
           "insert into plans (scope_kind, title, slug, status) values ('global','p1','claim-plan-one','active')",
           "insert into plans (scope_kind, title, slug, status) values ('global','p2','claim-plan-two','active')",
           "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 't1', 'todo', 100)",
           "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 2, 't2', 'todo', 100)",
           "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 't3', 'todo', 200)",
           "insert into plan_steps (plan_id, ordinal, body) values (1, 1, 'a step')",
       }) {
    REQUIRE(conn->execute(statement).has_value());
  }
  return fx;
}

auto digest_of(const cfg::policy_values& values) -> std::string {
  return cfg::policy_digest(values);
}

} // namespace

TEST_CASE("the default policy admits a coder claim without --model and records the default policy's digest",
          "[cmd][agent][claim][policy][1142]") {
  auto       fx       = make_fixture("claimdefault");
  auto const defaults = digest_of(cfg::policy_values{});

  // No config file at all.
  auto const bare = fx.claim("task:1", "coder", std::nullopt);
  INFO("stderr:\n" << bare.err);
  CHECK(bare.code == 0);
  CHECK(fx.scalar("select model is null from agent_work_claims where id = 1") == "1");
  CHECK(fx.newest_digest() == defaults);

  // A config with no policy table is the same policy.
  fx.write_config("[queue]\nslots = 1\n");
  CHECK(fx.claim("task:2", "coder", std::nullopt).code == 0);
  CHECK(fx.newest_digest() == defaults);

  // The digest is recorded for every role and entity kind, not only coder task claims.
  CHECK(fx.claim("plan:1", std::nullopt, std::nullopt).code == 0);
  CHECK(fx.newest_digest() == defaults);
  CHECK(fx.claim("plan_step:1", "planner", std::nullopt).code == 0);
  CHECK(fx.newest_digest() == defaults);
  CHECK(fx.claim("task:3", "reviewer", std::nullopt).code == 0);
  CHECK(fx.newest_digest() == defaults);
  CHECK(fx.scalar("select count(*) from agent_work_claims where policy_digest is null") == "0");
}

TEST_CASE("pull records the policy digest and, by default, takes a coder pull without --model",
          "[cmd][agent][claim][policy][1142]") {
  auto       fx     = make_fixture("pulldefault");
  auto const pulled = fx.pull(1, "coder", std::nullopt);
  INFO("stderr:\n" << pulled.err);
  CHECK(pulled.code == 0);
  CHECK(fx.scalar("select entity_kind || ':' || entity_id from agent_work_claims where id = 1") == "task:1");
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{}));

  fx.write_config("[policy]\nreview_required = true\n");
  CHECK(fx.pull(1, "coder", "m").code == 0);
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{.review_required = true}));
}

TEST_CASE("require_model: a coder claim without --model is refused, names the flag, and writes nothing",
          "[cmd][agent][claim][policy][1142]") {
  auto fx = make_fixture("reqmodel");
  fx.write_config("[policy]\nrequire_model = true\n");
  auto const before = fx.state();

  auto const missing = fx.claim("task:1", "coder", std::nullopt);
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  CHECK(missing.err == std::format("error: {}\n", k_require_model_text));
  CHECK(fx.state() == before);

  // An empty model is a missing model.
  auto const empty = fx.claim("task:1", "coder", "");
  CHECK(empty.code == 1);
  CHECK(empty.err.contains("--model"));
  CHECK(fx.state() == before);

  // The refusal is the same for every entity kind a coder can claim, and in JSON mode it keeps its tag.
  CHECK(fx.claim("plan:1", "coder", std::nullopt).code == 1);
  CHECK(fx.claim("plan_step:1", "coder", std::nullopt).code == 1);
  auto const json = fx.agent({"claim", "--entity", "task:1", "--role", "coder", "--json"});
  CHECK(json.code == 1);
  CHECK(json.out.contains("PolicyUnmet"));
  CHECK(fx.state() == before);

  // A model satisfies it, the claim stores that model verbatim, and the digest is the policy's own.
  auto const named = fx.claim("task:1", "coder", "m-1");
  INFO("stderr:\n" << named.err);
  CHECK(named.code == 0);
  CHECK(fx.scalar("select model from agent_work_claims where id = 1") == "m-1");
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{.require_model = true}));
  CHECK(fx.newest_digest() != digest_of(cfg::policy_values{}));

  // Roles other than coder are not bound by it, and neither is a claim that names no role.
  CHECK(fx.claim("task:2", "reviewer", std::nullopt).code == 0);
  CHECK(fx.claim("plan:2", "planner", std::nullopt).code == 0);
  CHECK(fx.claim("plan:1", std::nullopt, std::nullopt).code == 0);
}

TEST_CASE("require_model: a coder pull without --model is refused and reads and writes nothing",
          "[cmd][agent][claim][policy][1142]") {
  auto fx = make_fixture("reqpull");
  fx.write_config("[policy]\nrequire_model = true\n");
  auto const before = fx.state();

  auto const missing = fx.pull(1, "coder", std::nullopt);
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
  CHECK(missing.err == "error: pull: policy require_model = true: coder claims must say which model they use "
                       "(add --model '<id>')\n");
  CHECK(fx.pull(1, "coder", "").code == 1);
  CHECK(fx.state() == before);

  // A pull records a coder action when it names no role, an unrecognised one or `review`, so it is bound as a coder pull.
  // (A plain `claim` that names no role keeps its exemption, above: the claim records no action kind of its own.)
  for (auto const role : {std::optional<std::string_view>{}, std::optional<std::string_view>{"not-a-role"},
                          std::optional<std::string_view>{"review"}, std::optional<std::string_view>{"run_submitted"}}) {
    INFO("role: " << role.value_or("(none)"));
    auto const refused = fx.pull(1, role, std::nullopt);
    CHECK(refused.code == 1);
    CHECK(refused.err.contains("require_model = true"));
    CHECK(fx.state() == before);
  }

  // A reviewer's pull is not bound, and a named model satisfies the coder's.
  CHECK(fx.pull(1, "reviewer", std::nullopt).code == 0);
  auto const named = fx.pull(1, "coder", "m-2");
  INFO("stderr:\n" << named.err);
  CHECK(named.code == 0);
  CHECK(fx.scalar("select model from agent_work_claims order by id desc limit 1") == "m-2");
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{.require_model = true}));
}

TEST_CASE("require_model: a role-less pull that names a model is a coder pull and is recorded as one",
          "[cmd][agent][claim][policy][1142]") {
  auto fx = make_fixture("reqpullnorole");
  fx.write_config("[policy]\nrequire_model = true\n");

  auto const pulled = fx.pull(1, std::nullopt, "m-0");
  INFO("stderr:\n" << pulled.err);
  CHECK(pulled.code == 0);
  CHECK(fx.scalar("select model from agent_work_claims order by id desc limit 1") == "m-0");
  CHECK(fx.scalar("select count(*) from agent_actions where action_kind = 'coder'") == "1");
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{.require_model = true}));
}

TEST_CASE("require_model in an association's policy table binds only that association's tasks",
          "[cmd][agent][claim][policy][1142]") {
  auto fx = make_fixture("reqassoc");
  fx.sql("insert into associations (id, slug, name, kind) values (1, 'acme', 'Acme', 'org')");
  fx.sql("update plans set scope_kind = 'association', scope_id = 1 where id = 1");
  fx.write_config("[associations.\"acme\".policy]\nrequire_model = true\n");

  // Task 1 is on plan 1, in the association; task 2 is on a global plan.
  auto const bound = fx.claim("task:1", "coder", std::nullopt);
  CHECK(bound.code == 1);
  CHECK(bound.err.contains("--model"));
  CHECK(fx.pull(1, "coder", std::nullopt).code == 1);
  CHECK(fx.claim("plan_step:1", "coder", std::nullopt).code == 1);
  CHECK(fx.scalar("select count(*) from agent_work_claims") == "0");

  CHECK(fx.claim("task:2", "coder", std::nullopt).code == 0);
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{}));
  CHECK(fx.claim("task:1", "coder", "m").code == 0);
  CHECK(fx.newest_digest() == digest_of(cfg::policy_values{.require_model = true}));

  // The association's value wins over the top level in both directions.
  fx.sql("update agent_work_claims set status = 'released' where entity_id = 1 and entity_kind = 'task'");
  fx.sql("update tasks set status = 'todo' where id = 1");
  fx.write_config("[policy]\nrequire_model = true\n[associations.\"acme\".policy]\nrequire_model = false\n");
  CHECK(fx.claim("task:1", "coder", std::nullopt).code == 0);
  CHECK(fx.claim("task:3", "coder", std::nullopt).code == 0);
}

TEST_CASE("no environment variable switches require_model off", "[cmd][agent][claim][policy][1142]") {
  auto fx = make_fixture("reqenv");
  fx.write_config("[policy]\nrequire_model = true\n");
  for (std::string_view const name : {"PLANAR_REQUIRE_MODEL", "PLANAR_POLICY_REQUIRE_MODEL", "REQUIRE_MODEL", "PLANAR_POLICY"}) {
    INFO("variable " << name);
    auto const refused = fx.claim("task:1", "coder", std::nullopt, {pinned_var{.name = std::string{name}, .value = "false"}});
    CHECK(refused.code == 1);
    CHECK(refused.err.contains("--model"));
  }
  CHECK(fx.scalar("select count(*) from agent_work_claims") == "0");
}

TEST_CASE("a policy that cannot be applied refuses a coder claim and lets every other claim through without a digest",
          "[cmd][agent][claim][policy][1142]") {
  auto       fx     = make_fixture("claimbad");
  auto const before = fx.state();

  fx.write_config("[policy]\nafter_review = \"dispatch\"\nrequire_model = false\n");
  auto const unknown = fx.claim("task:1", "coder", "m");
  CHECK(unknown.code == 1);
  CHECK(unknown.err.contains("claim: the policy in config.toml cannot be applied"));
  CHECK(unknown.err.contains("policy.after_review"));
  CHECK(unknown.err.contains("planar config validate"));
  auto const pulled = fx.pull(1, "coder", "m");
  CHECK(pulled.code == 1);
  CHECK(pulled.err.contains("pull: the policy in config.toml cannot be applied"));
  CHECK(fx.state() == before);

  fx.write_config("[policy\nrequire_model = true\n");
  CHECK(fx.claim("task:1", "coder", "m").code == 1);
  CHECK(fx.state() == before);

  // Unrelated work goes on: no digest is recorded, because no policy could be applied.
  CHECK(fx.claim("task:1", "reviewer", std::nullopt).code == 0);
  CHECK(fx.newest_digest() == "NULL");
  CHECK(fx.claim("plan:2", std::nullopt, std::nullopt).code == 0);
  CHECK(fx.newest_digest() == "NULL");
}

TEST_CASE("the recorded digest is stable for one policy and differs between policies", "[cmd][agent][claim][policy][1142]") {
  auto fx          = make_fixture("claimdigest");
  auto release_all = [&] {
    fx.sql("update agent_work_claims set status = 'released' where status = 'active'");
    fx.sql("update tasks set status = 'todo'");
  };

  fx.write_config("[policy]\nreview_required = true\n");
  REQUIRE(fx.claim("task:1", "coder", std::nullopt).code == 0);
  auto const first = fx.newest_digest();
  release_all();
  REQUIRE(fx.claim("task:1", "coder", std::nullopt).code == 0);
  CHECK(fx.newest_digest() == first);
  CHECK(first == digest_of(cfg::policy_values{.review_required = true}));
  CHECK(first.size() == 64);
  release_all();

  fx.write_config("[policy]\nreview_required = true\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n");
  REQUIRE(fx.claim("task:1", "coder", std::nullopt).code == 0);
  auto const second = fx.newest_digest();
  CHECK(second != first);
  CHECK(second == digest_of(cfg::policy_values{
                      .review_required = true, .task_gate = std::vector<std::string>{"make", "test"}, .use_queue = true}));
  release_all();

  // The same effective values written another way are the same digest.
  fx.write_config("[policy]\nreview_required = false\nuse_queue = true\ntask_gate = [\"make\", \"test\"]\n"
                  "[associations.\"nowhere\".policy]\nreview_required = true\n");
  REQUIRE(fx.pull(1, "coder", std::nullopt).code == 0);
  CHECK(fx.newest_digest() ==
        digest_of(cfg::policy_values{.task_gate = std::vector<std::string>{"make", "test"}, .use_queue = true}));
}
