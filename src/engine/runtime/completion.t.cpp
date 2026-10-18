// completion.t.cpp: `planar.engine.runtime.completion` and the `check_completion`
// hook of `agentatomic::task_policy` (plan 1142, task mo-policy-enforce-complete;
// test spec 695, "Review and gate refusals at complete").
//
// The gate evidence arrives through an injected lookup, so these cases build the
// rows by hand and assert on exactly what the engine compares: argv element by
// element, the pass flag, the claim's token (the lookup is asked for the
// completing claim's token only) and the claim's directories. A review is
// seeded either through `record_review` (the verb's path) or as a raw row (the
// only way to get a same-session or NULL-verdict row, which the verb cannot write).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.completion;

namespace {

namespace aa     = planar::engine::runtime::agentactivity;
namespace atomic = planar::engine::runtime::agentatomic;
namespace comp   = planar::engine::runtime::completion;

struct scratch_db {
  std::filesystem::path path;

  scratch_db()
      : path(std::filesystem::temp_directory_path() / std::format("planar_completion_test_{}_{}.db",
                                                                  std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                  reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db(const scratch_db&)            = delete;
  scratch_db& operator=(const scratch_db&) = delete;
  ~scratch_db() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
  }
};

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

auto scalar_int(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// The task transition matrix is not under test here: any move is legal.
auto policy_without_requirements() -> atomic::task_policy {
  return atomic::task_policy{
      .check_transition = [](std::string_view, std::string_view) -> std::expected<void, aa::agent_error> { return {}; },
      .recompute_plan   = [](planar::db::connection&, std::int64_t) -> std::expected<void, aa::agent_error> { return {}; },
      .clear_unblocked_dependents = [](planar::db::connection&, std::int64_t) -> std::expected<void, aa::agent_error> {
        return {};
      },
      .unready_reasons = [](planar::db::connection&, std::int64_t) -> std::expected<std::vector<std::string>, aa::agent_error> {
        return {};
      },
  };
}

struct world {
  scratch_db             scratch;
  planar::db::connection conn = [this] {
    auto opened = planar::db::connection::open(scratch.path.string());
    REQUIRE(opened.has_value());
    REQUIRE(planar::db::apply_all(*opened).has_value());
    return std::move(*opened);
  }();
  aa::claim claim;

  /// One plan with one task, pulled under `vendor_session`; the claim records `repo_root` and `worktree`.
  explicit world(std::string_view vendor_session = "claude:coder-1", std::optional<std::string_view> repo_root = "/work/repo",
                 std::optional<std::string_view> worktree = std::nullopt) {
    exec(conn, "insert into sessions (vendor) values ('test')");
    exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','p','test-plan','active')");
    exec(conn, "insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 't', 'todo', 100)");
    auto const pulled = atomic::pull_next(conn,
                                          atomic::pull_args{.plan_id    = 1,
                                                            .session_id = scalar_int(conn, "select id from sessions limit 1"),
                                                            .vendor     = "claude",
                                                            .vendor_session_id = vendor_session,
                                                            .role              = "coder"},
                                          policy_without_requirements());
    REQUIRE(pulled.has_value());
    REQUIRE(pulled->acquired.has_value());
    claim = *pulled->acquired;
    if (repo_root.has_value()) {
      exec(conn, std::format("update agent_work_claims set repo_root = '{}' where id = {}", *repo_root, claim.id));
    }
    if (worktree.has_value()) {
      exec(conn, std::format("update agent_work_claims set worktree_path = '{}' where id = {}", *worktree, claim.id));
    }
  }

  void review(aa::review_verdict verdict, std::string_view session) {
    auto const done = atomic::record_review(conn, atomic::review_args{.reviewed_claim_token    = claim.claim_token,
                                                                      .reviewer_vendor_session = session,
                                                                      .vendor                  = "claude",
                                                                      .verdict                 = verdict,
                                                                      .summary                 = "looked"});
    REQUIRE(done.has_value());
  }

  /// A raw review row in the CLAIM'S OWN session, with `verdict_sql` (`'approve'` or `null`).
  void seed_same_session_review(std::string_view verdict_sql) {
    exec(conn, std::format("insert into agent_actions (session_id, claim_id, action_kind, vendor, summary, verdict) "
                           "values ({}, {}, 'review', 'claude', 'self', {})",
                           claim.session_id, claim.id, verdict_sql));
  }
};

auto kinds(const std::vector<comp::unmet_requirement>& unmet) -> std::vector<comp::requirement_kind> {
  std::vector<comp::requirement_kind> out;
  for (auto const& item : unmet) {
    out.push_back(item.kind);
  }
  return out;
}

using rk = comp::requirement_kind;

/// A lookup keyed by tag, as the queue's is: it returns `runs` for `runs_tag` and nothing for any other tag, counts its
/// calls and records the tags it was asked about. When `failure` is set it fails with that reason instead.
struct fake_lookup {
  std::vector<comp::gate_run> runs;
  std::string                 runs_tag; ///< The tag `runs` are recorded under; empty matches the claim under test.
  std::optional<std::string>  failure;
  int                         calls = 0;
  std::vector<std::string>    tags;

  auto bind(std::string_view claim_token) -> comp::gate_run_lookup {
    return [this,
            claim = std::string{claim_token}](std::string_view tag) -> std::expected<std::vector<comp::gate_run>, std::string> {
      ++calls;
      tags.emplace_back(tag);
      if (failure.has_value()) {
        return std::unexpected(*failure);
      }
      if (tag != (runs_tag.empty() ? claim : runs_tag)) {
        return std::vector<comp::gate_run>{};
      }
      return runs;
    };
  }
};

auto gate_make_test() -> comp::completion_requirements {
  return comp::completion_requirements{.review_required = false, .task_gate = std::vector<std::string>{"make", "test"}};
}

auto run(std::vector<std::string> argv, std::string cwd, bool passed = true) -> comp::gate_run {
  return comp::gate_run{.argv = std::move(argv), .cwd = std::move(cwd), .passed = passed};
}

auto unmet_for(world& w, const comp::completion_requirements& requirements, fake_lookup& lookup)
    -> std::vector<comp::unmet_requirement> {
  auto const unmet =
      comp::unmet_completion_requirements(w.conn, w.claim.claim_token, requirements, lookup.bind(w.claim.claim_token));
  REQUIRE(unmet.has_value());
  return *unmet;
}

} // namespace

TEST_CASE("no policy key set reads nothing and asks nobody", "[completion][1142]") {
  world       w;
  fake_lookup lookup;
  CHECK_FALSE(comp::completion_requirements{}.any());
  CHECK(unmet_for(w, comp::completion_requirements{}, lookup).empty());
  CHECK(lookup.calls == 0);
  // Not even an unknown claim is looked up when there is nothing to check.
  auto const unknown = comp::unmet_completion_requirements(w.conn, "0123456789abcdef0123456789abcdef",
                                                           comp::completion_requirements{}, lookup.bind(""));
  REQUIRE(unknown.has_value());
  CHECK(unknown->empty());
  CHECK(lookup.calls == 0);
}

TEST_CASE("review_required without any review is unmet and a review in the claim's own session does not count",
          "[completion][review][1142]") {
  world       w;
  fake_lookup lookup;
  auto const  required = comp::completion_requirements{.review_required = true};

  CHECK(kinds(unmet_for(w, required, lookup)) == std::vector{rk::review_missing});

  // The verb cannot write these; the gate must still not trust them.
  w.seed_same_session_review("'approve'");
  CHECK(kinds(unmet_for(w, required, lookup)) == std::vector{rk::review_not_independent});
  CHECK(lookup.calls == 0);
}

TEST_CASE("a review row with a NULL verdict is no review", "[completion][review][1142]") {
  world       w;
  fake_lookup lookup;
  w.seed_same_session_review("null");
  auto const unmet = unmet_for(w, comp::completion_requirements{.review_required = true}, lookup);
  CHECK(kinds(unmet) == std::vector{rk::review_missing});
}

TEST_CASE("an independent approval satisfies review_required; the latest verdict wins", "[completion][review][1142]") {
  world       w;
  fake_lookup lookup;
  auto const  required = comp::completion_requirements{.review_required = true};

  w.review(aa::review_verdict::request_changes, "claude:reviewer-1");
  auto const changes = unmet_for(w, required, lookup);
  REQUIRE(kinds(changes) == std::vector{rk::review_changes});
  CHECK(changes.front().text.contains("requests changes"));

  w.review(aa::review_verdict::approve, "claude:reviewer-2");
  CHECK(unmet_for(w, required, lookup).empty());

  // A later request-changes supersedes the approval.
  w.review(aa::review_verdict::request_changes, "claude:reviewer-2");
  CHECK(kinds(unmet_for(w, required, lookup)) == std::vector{rk::review_changes});
}

TEST_CASE("a gate is met only by a passing run with the exact argv in the claim's own directory", "[completion][gate][1142]") {
  world       w{"claude:coder-1", "/work/repo", "/work/wt"};
  fake_lookup lookup;
  auto const  gate = gate_make_test();

  // No run at all.
  CHECK(kinds(unmet_for(w, gate, lookup)) == std::vector{rk::gate_missing});
  CHECK(lookup.tags.front() == w.claim.claim_token);

  // Near misses on argv: extra element, prefix, shell-wrapped, case, whitespace, order.
  lookup.runs = {run({"make", "test", "-j"}, "/work/repo"),
                 run({"make"}, "/work/repo"),
                 run({"sh", "-c", "make test"}, "/work/repo"),
                 run({"Make", "test"}, "/work/repo"),
                 run({"make", "test "}, "/work/repo"),
                 run({"test", "make"}, "/work/repo"),
                 run({"make test"}, "/work/repo")};
  CHECK(kinds(unmet_for(w, gate, lookup)) == std::vector{rk::gate_missing});

  // Exact argv, but failed.
  lookup.runs = {run({"make", "test"}, "/work/repo", false)};
  CHECK(kinds(unmet_for(w, gate, lookup)) == std::vector{rk::gate_missing});

  // Exact argv and passing, but elsewhere: another checkout, a subdirectory, the parent.
  lookup.runs = {run({"make", "test"}, "/elsewhere"), run({"make", "test"}, "/work/repo/sub"), run({"make", "test"}, "/work"),
                 run({"make", "test"}, "")};
  CHECK(kinds(unmet_for(w, gate, lookup)) == std::vector{rk::gate_missing});

  // The repository root, with a trailing separator and an unnormalized spelling, counts.
  lookup.runs = {run({"make", "test"}, "/work/repo/")};
  CHECK(unmet_for(w, gate, lookup).empty());
  lookup.runs = {run({"make", "test"}, "/work/./repo")};
  CHECK(unmet_for(w, gate, lookup).empty());
  // So does the worktree path.
  lookup.runs = {run({"make", "test"}, "/work/wt")};
  CHECK(unmet_for(w, gate, lookup).empty());
}

TEST_CASE("a gate is looked up by the claim's own token as the tag, and another tag's runs do not count", "[completion][gate]") {
  world       w{"claude:coder-1", "/work/repo", std::nullopt};
  fake_lookup lookup;
  lookup.runs     = {run({"make", "test"}, "/work/repo")};
  lookup.runs_tag = "ffffffffffffffffffffffffffffffff";
  CHECK(kinds(unmet_for(w, gate_make_test(), lookup)) == std::vector{rk::gate_missing});
  REQUIRE(lookup.tags.size() == 1);
  CHECK(lookup.tags.front() == w.claim.claim_token);
  lookup.runs_tag = w.claim.claim_token;
  CHECK(unmet_for(w, gate_make_test(), lookup).empty());
}

TEST_CASE("a lookup that fails is gate_unreadable with the reason, never gate_missing", "[completion][gate]") {
  world       w{"claude:coder-1", "/work/repo", std::nullopt};
  fake_lookup lookup;
  lookup.runs      = {run({"make", "test"}, "/work/repo")};
  lookup.failure   = "/q/build-queue.db: database is locked";
  auto const unmet = unmet_for(w, gate_make_test(), lookup);
  REQUIRE(unmet.size() == 1);
  CHECK(unmet.front().kind == rk::gate_unreadable);
  CHECK(unmet.front().text == "task_gate: the build queue database cannot be read: /q/build-queue.db: database is locked");
}

TEST_CASE("a claim that recorded no location can show no gate run", "[completion][gate][1142]") {
  world       w{"claude:coder-1", std::nullopt, std::nullopt};
  fake_lookup lookup;
  lookup.runs = {run({"make", "test"}, "/work/repo")};
  CHECK(kinds(unmet_for(w, gate_make_test(), lookup)) == std::vector{rk::gate_no_locality});
  CHECK(lookup.calls == 0);

  // A relative recorded path is not a location either.
  world relative{"claude:coder-1", "relative/repo", std::nullopt};
  CHECK(kinds(unmet_for(relative, gate_make_test(), lookup)) == std::vector{rk::gate_no_locality});
}

TEST_CASE("directories compare after normalization and never as prefixes", "[completion][gate][1142]") {
  CHECK(comp::same_directory("/a/b", "/a/b/"));
  CHECK(comp::same_directory("/a/b", "/a/./b"));
  CHECK(comp::same_directory("/a/b", "/a/c/../b"));
  CHECK_FALSE(comp::same_directory("/a/b", "/a/b/c"));
  CHECK_FALSE(comp::same_directory("/a/b", "/a/bc"));
  CHECK_FALSE(comp::same_directory("/a/b", "/A/b"));
  CHECK_FALSE(comp::same_directory("", ""));
  CHECK_FALSE(comp::same_directory("a/b", "a/b"));
  CHECK_FALSE(comp::normalize_directory("").has_value());
  CHECK_FALSE(comp::normalize_directory("rel").has_value());
}

TEST_CASE("a review and a gate are both reported, review first", "[completion][1142]") {
  world       w;
  fake_lookup lookup;
  auto        both     = gate_make_test();
  both.review_required = true;
  CHECK(kinds(unmet_for(w, both, lookup)) == std::vector{rk::review_missing, rk::gate_missing});
}

TEST_CASE("complete_work runs the check, refuses without writing, and fail, release and block never call it",
          "[completion][atomic][1142]") {
  world w;
  auto  policy            = policy_without_requirements();
  int   calls             = 0;
  bool  in_tx             = false;
  bool  refuse            = true;
  auto  expected_token    = w.claim.claim_token;
  policy.check_completion = [&](planar::db::connection& conn, const aa::claim& held) -> std::expected<void, aa::agent_error> {
    ++calls;
    in_tx = conn.in_transaction();
    CHECK(held.claim_token == expected_token);
    return refuse ? std::unexpected(aa::agent_error::completion_unmet) : std::expected<void, aa::agent_error>{};
  };

  auto const refused = atomic::complete_work(w.conn, w.claim.claim_token, "done", policy);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == aa::agent_error::completion_unmet);
  CHECK(aa::error_name(refused.error()) == "PolicyUnmet");
  CHECK(calls == 1);
  CHECK(in_tx);
  // Nothing was written: the task is still doing and the claim is still active.
  CHECK(scalar_int(w.conn, "select count(*) from tasks where status = 'doing'") == 1);
  CHECK(scalar_int(w.conn, "select count(*) from agent_work_claims where status = 'active'") == 1);

  // The other terminal verbs do not consult it.
  auto const released = atomic::release_work(w.conn, w.claim.claim_token, "giving up", policy);
  REQUIRE(released.has_value());
  CHECK(calls == 1);

  refuse = false;
  world other;
  expected_token      = other.claim.claim_token;
  auto const accepted = atomic::complete_work(other.conn, other.claim.claim_token, "done", policy);
  REQUIRE(accepted.has_value());
  CHECK(calls == 2);
  CHECK(scalar_int(other.conn, "select count(*) from tasks where status = 'done'") == 1);
}

TEST_CASE("an unbound check admits every completion", "[completion][atomic][1142]") {
  world      w;
  auto       policy = policy_without_requirements();
  auto const done   = atomic::complete_work(w.conn, w.claim.claim_token, "done", policy);
  REQUIRE(done.has_value());
}
