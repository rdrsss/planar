// @file closeout.t.cpp
// @brief Engine-level tests for `planar.engine.planning.closeout` (plan 996,
// task 6317).
//
// The CLI-visible bytes are pinned in `src/cmd/planar/plan_closeout_leaf.t.cpp`.
// This file covers what a leaf test cannot reach cleanly: the apply
// transaction's rollback on a failed audit INSERT, the three counting
// queries' disagreement about claim `status`, and the git-probe note
// composition, each driven directly against a scratch database.
//
// ## EVERY FIXTURE ASSERTS ITS OWN SHAPE BEFORE ANYTHING IS COMPARED
//
// A closeout fixture that seeded nothing reports `ready:true` with all-zero
// counts — which is ALSO the correct answer for a genuinely empty plan. So
// an absence assertion here can pass because the fixture matched nothing,
// and the paired presence assertion is what tells the two apart. Every
// blocking case below is paired with a case where the same rule does NOT
// fire, and the seeds go through `REQUIRE`d SQL rather than being assumed.
//
// ## ORACLE PROVENANCE
//
// Every expected string was captured from `zig/zig-out/bin/planar` built at
// this cycle's base, in a pinned scratch arena (`PLANAR_DB` under a temp
// root — never the operator's database, which matters more here than for a
// read verb: this one CLOSES PLANS). Exit codes were read from the command
// itself with stdout and stderr redirected to separate files, never through
// a pipe.
//
// A 23-arm differential replayed the whole capture against the built C++
// binary in a second identically-seeded arena: all 23 agreed on stdout,
// stderr, exit code and every asserted row of `plans`, `tasks`,
// `agent_work_claims` and `audit_log`.
//
// ## THE ONE DELIBERATE DIVERGENCE
//
// The advisory git layer is DEAD in the oracle — see closeout.cppm's
// DIVERGENCE section — so `the git probes compose each note arm` below has
// NO oracle counterpart to compare against and is written from
// `closeout.zig`'s source instead. It is the only test in this file that is
// not a captured comparison, and it is called out here so nobody mistakes
// it for one.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.closeout;

namespace {

namespace co = planar::engine::planning::closeout;

/// @brief A scratch database path, removed with its sidecars on destruction.
struct scratch_db_path {
  std::filesystem::path path_;
  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_closeout_eng_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;
  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(path_.string() + suffix, ec);
    }
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

auto scalar(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto text_scalar(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

/// @brief Seed a plan, optionally under a parent, and return its id.
auto seed_plan(planar::db::connection& conn, std::string_view slug, std::string_view status,
               std::optional<std::int64_t> parent = std::nullopt) -> std::int64_t {
  if (parent.has_value()) {
    exec(conn, std::format("insert into plans (scope_kind, title, slug, status, parent_plan_id) "
                           "values ('global','{0}','{0}','{1}',{2})",
                           slug, status, *parent));
  } else {
    exec(conn,
         std::format("insert into plans (scope_kind, title, slug, status) values ('global','{0}','{0}','{1}')", slug, status));
  }
  auto const id = scalar(conn, std::format("select id from plans where slug='{}'", slug));
  // Assert the seed landed as asked rather than trusting the INSERT.
  REQUIRE(text_scalar(conn, std::format("select status from plans where id={}", id)) == status);
  return id;
}

/// @brief Seed a task and return its id.
auto seed_task(planar::db::connection& conn, std::int64_t plan_id, std::string_view slug, std::string_view status)
    -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, plan_id, title, slug, status) "
                         "values ('global',{},'{}','{}','{}')",
                         plan_id, slug, slug, status));
  auto const id = scalar(conn, std::format("select id from tasks where slug='{}'", slug));
  REQUIRE(text_scalar(conn, std::format("select status from tasks where id={}", id)) == status);
  return id;
}

/// @brief Seed a claim. `lease` is a full ISO-8601 stamp so the caller
/// chooses live vs stale explicitly rather than by arithmetic on `now`.
auto seed_claim(planar::db::connection& conn, std::string_view token, std::int64_t task_id, std::string_view status,
                std::string_view lease, std::optional<std::string_view> repo_root = std::nullopt,
                std::optional<std::string_view> branch = std::nullopt, std::optional<std::string_view> sha = std::nullopt)
    -> void {
  if (scalar(conn, "select count(*) from sessions") == 0) {
    exec(conn, "insert into sessions (vendor) values ('test')");
  }
  auto const quoted = [](std::optional<std::string_view> value) -> std::string {
    return value.has_value() ? std::format("'{}'", *value) : std::string{"NULL"};
  };
  exec(conn, std::format("insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, "
                         "vendor, lease_expires_at, repo_root, branch, head_sha_at_claim) "
                         "values ('{}', 1, 'task', {}, '{}', 'test', '{}', {}, {}, {})",
                         token, task_id, status, lease, quoted(repo_root), quoted(branch), quoted(sha)));
}

constexpr std::string_view k_future = "2099-01-01T00:00:00.000Z";
constexpr std::string_view k_past   = "2000-01-01T00:00:00.000Z";

/// @brief The synthetic entry a live evaluation with no locality emits.
constexpr std::string_view k_no_attribution =
    "no commit attribution — inconclusive (hardens once session-commit capture is wired)";

} // namespace

TEST_CASE("an empty active plan is ready, and emits the no-attribution synthetic", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "empty", "active");

  // Fixture shape, asserted before any comparison: no tasks, no descendants,
  // no claims. Without this the all-zero result below could not be told
  // apart from a query that silently matched nothing.
  REQUIRE(scalar(conn, "select count(*) from tasks") == 0);
  REQUIRE(scalar(conn, "select count(*) from agent_work_claims") == 0);

  auto const result = co::evaluate(conn, plan_id, false, false);
  REQUIRE(result.has_value());
  CHECK(result->ready);
  CHECK_FALSE(result->applied);
  CHECK(result->blocked_by.empty());
  CHECK(result->warnings.empty());
  CHECK(result->hard.tasks.open == 0);
  CHECK(result->hard.tasks.done == 0);
  CHECK(result->hard.tasks.cancelled == 0);
  CHECK_FALSE(result->epic_merge.has_value());

  // NOT empty: one synthetic entry. The already-terminal path below is the
  // one that returns an empty vector, and confusing the two is the mistake
  // this pair exists to catch.
  REQUIRE(result->git.size() == 1);
  CHECK(result->git[0].repo_root == "(none)");
  CHECK(result->git[0].note == k_no_attribution);
  CHECK_FALSE(result->git[0].branch.has_value());
  CHECK_FALSE(result->git[0].target_branch.has_value());

  // Dry-run wrote nothing.
  CHECK(text_scalar(conn, std::format("select status from plans where id={}", plan_id)) == "active");
}

TEST_CASE("a missing plan is not_found, and id 0 is not special", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan(conn, "present", "active");

  auto const missing = co::evaluate(conn, 999, false, false);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == co::closeout_error::not_found);

  // An omitted-optional id must not act on some row by accident.
  auto const zero = co::evaluate(conn, 0, true, false);
  REQUIRE_FALSE(zero.has_value());
  CHECK(zero.error() == co::closeout_error::not_found);
  CHECK(scalar(conn, "select count(*) from plans where status='done'") == 0);
}

TEST_CASE("open tasks block and cancelled tasks do not", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const blocked_plan = seed_plan(conn, "open-tasks", "active");
  seed_task(conn, blocked_plan, "still-todo", "todo");
  seed_task(conn, blocked_plan, "in-flight", "doing");
  seed_task(conn, blocked_plan, "held", "blocked");
  seed_task(conn, blocked_plan, "finished", "done");

  auto const blocked = co::evaluate(conn, blocked_plan, false, false);
  REQUIRE(blocked.has_value());
  CHECK_FALSE(blocked->ready);
  CHECK(blocked->hard.tasks.open == 3);
  CHECK(blocked->hard.tasks.done == 1);
  REQUIRE(blocked->blocked_by.size() == 1);
  CHECK(blocked->blocked_by[0] == "3 open task(s) on plan (todo/doing/blocked)");

  // The paired NON-firing case on the same rule: `cancelled` is terminal
  // HISTORY. It is counted and it does not block.
  auto const clean_plan = seed_plan(conn, "cancelled-only", "active");
  seed_task(conn, clean_plan, "abandoned-work", "cancelled");

  auto const clean = co::evaluate(conn, clean_plan, false, false);
  REQUIRE(clean.has_value());
  CHECK(clean->ready);
  CHECK(clean->blocked_by.empty());
  CHECK(clean->hard.tasks.open == 0);
  CHECK(clean->hard.tasks.cancelled == 1);
}

TEST_CASE("descendant tasks and descendant plans are both walked recursively", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const root  = seed_plan(conn, "root", "active");
  auto const child = seed_plan(conn, "child", "done", root);
  auto const grand = seed_plan(conn, "grand", "active", child);

  // A task two levels down must reach the roll-up, and a task on the root
  // itself must not be double-counted by the two summed queries.
  seed_task(conn, root, "root-task", "done");
  seed_task(conn, grand, "grand-task", "todo");

  auto const result = co::evaluate(conn, root, false, false);
  REQUIRE(result.has_value());
  CHECK(result->hard.tasks.done == 1); // not 2 — the root task is counted once
  CHECK(result->hard.tasks.open == 1);
  CHECK(result->hard.descendants.open == 1);     // `grand`
  CHECK(result->hard.descendants.terminal == 1); // `child`
  CHECK_FALSE(result->ready);

  // Both rules fired, and the reason order is rule order: tasks first.
  REQUIRE(result->blocked_by.size() == 2);
  CHECK(result->blocked_by[0] == "1 open task(s) on plan (todo/doing/blocked)");
  CHECK(result->blocked_by[1] == "1 open descendant plan(s) (draft/active/paused)");

  // The paired non-firing case: a leaf plan has no descendants at all.
  auto const leaf = co::evaluate(conn, grand, false, false);
  REQUIRE(leaf.has_value());
  CHECK(leaf->hard.descendants.open == 0);
  CHECK(leaf->hard.descendants.terminal == 0);
}

TEST_CASE("a live claim blocks and a stale claim only warns", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const live_plan = seed_plan(conn, "live", "active");
  auto const live_task = seed_task(conn, live_plan, "live-task", "done");
  seed_claim(conn, "live-token", live_task, "active", k_future);

  auto const live = co::evaluate(conn, live_plan, false, false);
  REQUIRE(live.has_value());
  CHECK_FALSE(live->ready);
  CHECK(live->hard.claims.live == 1);
  CHECK(live->hard.claims.stale == 0);
  REQUIRE(live->blocked_by.size() == 1);
  CHECK(live->blocked_by[0] == "1 live claim(s) still active on plan tasks");
  CHECK(live->warnings.empty());

  auto const stale_plan = seed_plan(conn, "stale", "active");
  auto const stale_task = seed_task(conn, stale_plan, "stale-task", "done");
  seed_claim(conn, "stale-token", stale_task, "active", k_past);

  auto const stale = co::evaluate(conn, stale_plan, false, false);
  REQUIRE(stale.has_value());
  CHECK(stale->ready); // the whole point: an expired lease is reconcilable
  CHECK(stale->blocked_by.empty());
  CHECK(stale->hard.claims.live == 0);
  CHECK(stale->hard.claims.stale == 1);
  REQUIRE(stale->warnings.size() == 1);
  CHECK(stale->warnings[0] == "1 stale/expired claim(s) on plan tasks — reconcilable, not blocking");
}

TEST_CASE("a non-active claim contributes git evidence but no claim count", "[engine][planning][closeout]") {
  // THE ASYMMETRY. `collect_claim_counts` filters `status='active'`; the two
  // locality queries do not. A port that filtered all three the same way
  // would look tidier and would be wrong — measured on the oracle, which
  // reported `claims.live:3` beside FOUR git-evidence entries on a fixture
  // whose fourth claim was `released`.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "asym", "active");
  auto const      task_id = seed_task(conn, plan_id, "asym-task", "done");

  seed_claim(conn, "released-token", task_id, "released", k_future, "/nonexistent/co6317/repo", "some-branch");

  // Fixture shape first: exactly one claim, and it is NOT active.
  REQUIRE(scalar(conn, "select count(*) from agent_work_claims") == 1);
  REQUIRE(scalar(conn, "select count(*) from agent_work_claims where status='active'") == 0);

  auto const result = co::evaluate(conn, plan_id, false, true);
  REQUIRE(result.has_value());
  CHECK(result->hard.claims.live == 0);
  CHECK(result->hard.claims.stale == 0);
  CHECK(result->ready); // a released claim does not block

  // ...and yet it produced a git-evidence row, and an epic roll-up.
  REQUIRE(result->git.size() == 1);
  CHECK(result->git[0].repo_root == "/nonexistent/co6317/repo");
  CHECK(result->git[0].branch == "some-branch");
  REQUIRE(result->epic_merge.has_value());
  CHECK(result->epic_merge->total_branches == 1);
}

TEST_CASE("the two locality queries use different distinct keys", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "locality", "active");
  auto const      task_id = seed_task(conn, plan_id, "locality-task", "done");

  constexpr std::string_view k_repo = "/nonexistent/co6317/locality";
  // Same (repo, branch), DIFFERENT sha: two git-evidence rows, ONE epic pair.
  seed_claim(conn, "loc-a", task_id, "released", k_future, k_repo, "shared-branch", "aaaa");
  seed_claim(conn, "loc-b", task_id, "released", k_future, k_repo, "shared-branch", "bbbb");
  // A NULL branch: contributes a git-evidence row, contributes NO epic pair.
  seed_claim(conn, "loc-c", task_id, "released", k_future, k_repo, std::nullopt, "cccc");
  // A NULL repo_root: contributes to NEITHER (both queries require it).
  seed_claim(conn, "loc-d", task_id, "released", k_future, std::nullopt, "orphan-branch", "dddd");

  REQUIRE(scalar(conn, "select count(*) from agent_work_claims") == 4);

  auto const result = co::evaluate(conn, plan_id, false, true);
  REQUIRE(result.has_value());
  CHECK(result->git.size() == 3); // a, b, c — d has no repo_root
  REQUIRE(result->epic_merge.has_value());
  CHECK(result->epic_merge->total_branches == 1); // a and b collapse; c and d excluded

  // Paired absence: with no locality at all the roll-up is UNSET, not a
  // zero-valued struct. An assertion that it is unset could otherwise pass
  // because `--check-merge` was never honoured.
  auto const bare_plan = seed_plan(conn, "bare", "active");
  auto const bare      = co::evaluate(conn, bare_plan, false, true);
  REQUIRE(bare.has_value());
  CHECK_FALSE(bare->epic_merge.has_value());
}

TEST_CASE("finalization slugs are counted and never block", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "finalization", "active");

  seed_task(conn, plan_id, "finalize-the-thing", "done");
  seed_task(conn, plan_id, "merge-the-thing", "done");
  seed_task(conn, plan_id, "reconcile-the-thing", "done");
  seed_task(conn, plan_id, "plain-thing", "done");
  // Prefix, not substring: `refinalize-` must NOT match `finalize-%`.
  seed_task(conn, plan_id, "refinalize-thing", "done");

  auto const result = co::evaluate(conn, plan_id, false, false);
  REQUIRE(result.has_value());
  CHECK(result->hard.finalization_tasks == 3);
  CHECK(result->hard.tasks.done == 5);
  CHECK(result->ready); // labelling only — it never reaches the gate

  // Paired absence on the same rule.
  auto const plain_plan = seed_plan(conn, "no-finalization", "active");
  seed_task(conn, plain_plan, "ordinary", "done");
  auto const plain = co::evaluate(conn, plain_plan, false, false);
  REQUIRE(plain.has_value());
  CHECK(plain->hard.finalization_tasks == 0);
}

TEST_CASE("apply marks the plan done and writes the closeout audit row", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "closable", "active");
  seed_task(conn, plan_id, "did-it", "done");
  seed_task(conn, plan_id, "dropped-it", "cancelled");

  REQUIRE(scalar(conn, "select count(*) from audit_log") == 0);

  auto const result = co::evaluate(conn, plan_id, true, false);
  REQUIRE(result.has_value());
  CHECK(result->ready);
  CHECK(result->applied);

  CHECK(text_scalar(conn, std::format("select status from plans where id={}", plan_id)) == "done");
  REQUIRE(scalar(conn, "select count(*) from audit_log") == 1);
  CHECK(text_scalar(conn, "select verb from audit_log") == "status_change");
  CHECK(text_scalar(conn, "select entity_kind from audit_log") == "plan");
  // The summary is what makes closeout distinguishable from
  // `plan update --status done` in the log, so its exact text is contract.
  CHECK(text_scalar(conn, "select summary from audit_log") ==
        std::format("closeout plan {}: → done; tasks done=1 cancelled=1; descendants terminal=0", plan_id));
}

TEST_CASE("apply on an already-terminal plan is a no-op with an EMPTY git vector", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "shut", "done");

  auto const result = co::evaluate(conn, plan_id, true, true);
  REQUIRE(result.has_value());
  CHECK(result->ready);
  CHECK_FALSE(result->applied);
  // The distinguishing property: EMPTY, where a live evaluation with no
  // locality returns the one-entry `(none)` synthetic. Same shape for
  // `abandoned`.
  CHECK(result->git.empty());
  CHECK_FALSE(result->epic_merge.has_value()); // even with check_merge on
  CHECK(scalar(conn, "select count(*) from audit_log") == 0);

  auto const abandoned_id = seed_plan(conn, "walked-away", "abandoned");
  auto const abandoned    = co::evaluate(conn, abandoned_id, true, true);
  REQUIRE(abandoned.has_value());
  CHECK(abandoned->ready);
  CHECK_FALSE(abandoned->applied);
  CHECK(abandoned->git.empty());
}

TEST_CASE("apply on a blocked plan writes nothing", "[engine][planning][closeout]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "not-yet", "active");
  seed_task(conn, plan_id, "outstanding", "todo");

  auto const result = co::evaluate(conn, plan_id, true, false);
  REQUIRE(result.has_value()); // NOT an error — a report with ready:false
  CHECK_FALSE(result->ready);
  CHECK_FALSE(result->applied);
  CHECK(text_scalar(conn, std::format("select status from plans where id={}", plan_id)) == "active");
  CHECK(scalar(conn, "select count(*) from audit_log") == 0);
}

TEST_CASE("a failed audit INSERT rolls the status change back", "[engine][planning][closeout]") {
  // The invariant worth a whole case: closeout must never leave a plan
  // CLOSED with no record of which path closed it. Break-probed by removing
  // the `write_failed` early return, which leaves the plan `done` and the
  // audit log empty.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "audit-fails", "active");
  seed_task(conn, plan_id, "complete", "done");

  exec(conn, "create trigger fail_closeout_audit before insert on audit_log "
             "begin select raise(abort, 'forced closeout audit failure'); end");

  auto const result = co::evaluate(conn, plan_id, true, false);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == co::closeout_error::write_failed);
  CHECK(text_scalar(conn, std::format("select status from plans where id={}", plan_id)) == "active");
  CHECK(scalar(conn, "select count(*) from audit_log") == 0);
}

TEST_CASE("the JSON renderer emits the oracle's exact bytes", "[engine][planning][closeout][render]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "render-json", "active");
  seed_task(conn, plan_id, "outstanding", "todo");

  auto const result = co::evaluate(conn, plan_id, false, false);
  REQUIRE(result.has_value());
  // Delimiter `J` throughout: these payloads contain `)"` — `blocked)"]`,
  // `wired)"}]` — which closes a bare `R"(` literal mid-string.
  CHECK(co::render_json(*result) ==
        R"J({"plan":1,"ready":false,"applied":false,"hard_evidence":{"tasks":{"open":1,"done":0,"cancelled":0},)J"
        R"J("descendants":{"open":0,"terminal":0},"claims":{"live":0,"stale":0},"finalization_tasks":0},)J"
        R"J("blocked_by":["1 open task(s) on plan (todo/doing/blocked)"],"git_evidence":[{"repo_root":"(none)",)J"
        R"J("branch":null,"target_branch":null,"base_merged":null,"branch_merged":null,)J"
        R"J("note":"no commit attribution — inconclusive (hardens once session-commit capture is wired)"}],)J"
        R"J("epic_merge":null,"warnings":[]}
)J");
}

TEST_CASE("the text renderer emits the oracle's exact bytes, banner defect included", "[engine][planning][closeout][render]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Blocked, with a finalization count and a stale-claim warning, so the two
  // conditional sections and the indented finalization line are all reached.
  auto const plan_id = seed_plan(conn, "render-text", "active");
  auto const done_id = seed_task(conn, plan_id, "merge-it", "done");
  seed_task(conn, plan_id, "outstanding", "todo");
  seed_claim(conn, "expired", done_id, "active", k_past);

  auto const blocked = co::evaluate(conn, plan_id, false, false);
  REQUIRE(blocked.has_value());
  CHECK(co::render_text(*blocked, true) ==
        "[dry-run] plan 1: NOT ready to close\n"
        "\n"
        "hard gate:\n"
        "  tasks:       open=1  done=1  cancelled=0\n"
        "    (finalization tasks: 1 — merge/reconcile/finalize-prefixed)\n"
        "  descendants: open=0  terminal=0\n"
        "  claims:      live=0  stale=1\n"
        "\n"
        "blocked by:\n"
        "  - 1 open task(s) on plan (todo/doing/blocked)\n"
        "\n"
        "warnings:\n"
        "  ! 1 stale/expired claim(s) on plan tasks — reconcilable, not blocking\n"
        "\n"
        "git evidence (advisory):\n"
        "  repo: (none)\n"
        "    branch:  (none)  target: (unknown)\n"
        "    note:    no commit attribution — inconclusive (hardens once session-commit capture is wired)\n");

  // THE BANNER DEFECT, pinned so it cannot be "fixed" without a failing
  // test forcing the conversation. This plan is `active` and this call
  // writes nothing, yet the oracle's middle arm says "already terminal".
  auto const ready_plan = seed_plan(conn, "render-ready", "active");
  auto const ready      = co::evaluate(conn, ready_plan, false, false);
  REQUIRE(ready.has_value());
  REQUIRE(ready->ready);
  REQUIRE_FALSE(ready->applied);
  CHECK(co::render_text(*ready, true).starts_with("[dry-run] plan 2: ready (already terminal — no change)\n"));
  // And with no `[dry-run] ` prefix when apply mode produced the same state.
  CHECK(co::render_text(*ready, false).starts_with("plan 2: ready (already terminal — no change)\n"));

  // The applied banner, and the absence of both optional sections.
  auto const applied = co::evaluate(conn, ready_plan, true, false);
  REQUIRE(applied.has_value());
  REQUIRE(applied->applied);
  auto const rendered = co::render_text(*applied, false);
  CHECK(rendered.starts_with("plan 2: marked done\n"));
  CHECK(rendered.find("blocked by:") == std::string::npos);
  CHECK(rendered.find("warnings:") == std::string::npos);
  // No finalization line when the count is zero.
  CHECK(rendered.find("finalization tasks") == std::string::npos);
}

TEST_CASE("the git probes compose each note arm", "[engine][planning][closeout][git]") {
  // NO ORACLE COUNTERPART — see this file's header. `closeout.zig`'s probes
  // are dead under Zig 0.16.0, so these expectations come from its SOURCE
  // and this port makes them real. Skipped when `git` is unavailable, the
  // posture the Zig tree's own capture tests already take.
  if (std::system("git --version > /dev/null 2>&1") != 0) {
    SUCCEED("git unavailable — skipping the probe arms");
    return;
  }

  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_closeout_git_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const repo = (root / "clone").string();
  auto const up   = (root / "up.git").string();
  auto const work = (root / "work").string();

  auto const sh = [](const std::string& line) { return std::system((line + " > /dev/null 2>&1").c_str()) == 0; };
  std::filesystem::create_directories(root);
  REQUIRE(sh(std::format("git init -q --bare -b main '{}'", up)));
  REQUIRE(sh(std::format("git init -q -b main '{}'", work)));
  REQUIRE(sh(std::format("git -C '{0}' config user.email t@t && git -C '{0}' config user.name t", work)));
  REQUIRE(sh(std::format("touch '{0}/a.txt' && git -C '{0}' add a.txt && git -C '{0}' commit -qm base", work)));
  REQUIRE(sh(std::format("git -C '{0}' remote add origin '{1}' && git -C '{0}' push -q origin main", work, up)));
  REQUIRE(sh(std::format("git clone -q '{}' '{}'", up, repo)));
  REQUIRE(sh(std::format("git -C '{0}' config user.email t@t && git -C '{0}' config user.name t", repo)));

  // The base sha, captured before any branch work, so `base_merged` has
  // something that genuinely IS an ancestor of main.
  std::string base_sha;
  {
    // NOT through `sh`: that helper appends its own `> /dev/null`, which
    // would win over this redirect and leave the file empty. The
    // `base_sha.size() == 40` guard below caught exactly that on the first
    // run — a fixture whose sha never landed would have driven every
    // `base_merged` arm with an empty string and reported `false`
    // everywhere, which reads as a plausible result.
    auto const sha_file = (root / "sha").string();
    REQUIRE(std::system(std::format("git -C '{}' rev-parse HEAD > '{}' 2>/dev/null", repo, sha_file).c_str()) == 0);
    std::ifstream in{sha_file};
    std::getline(in, base_sha);
  }
  REQUIRE(base_sha.size() == 40);

  REQUIRE(sh(std::format("git -C '{0}' checkout -q -b merged-branch && touch '{0}/b.txt' && git -C '{0}' add b.txt "
                         "&& git -C '{0}' commit -qm b",
                         repo)));
  REQUIRE(sh(std::format("git -C '{0}' checkout -q main && git -C '{0}' merge -q --no-ff merged-branch -m merge", repo)));
  REQUIRE(sh(std::format("git -C '{0}' checkout -q -b unmerged-branch && touch '{0}/c.txt' && git -C '{0}' add c.txt "
                         "&& git -C '{0}' commit -qm c",
                         repo)));
  // TASK 6321: a branch name past the oracle's 117-byte ceiling. Before the
  // fix, `branch_exists` refused to probe anything whose `refs/heads/<name>`
  // exceeded a 128-byte buffer and reported it ABSENT -- a closeout saying
  // "the branch is gone" about a branch that is right there.
  std::string const long_branch(120, 'x');
  REQUIRE(sh(std::format("git -C '{0}' checkout -q -b {1} && touch '{0}/d.txt' && git -C '{0}' add d.txt "
                         "&& git -C '{0}' commit -qm d",
                         repo, long_branch)));
  REQUIRE(sh(std::format("git -C '{}' checkout -q main", repo)));
  // Fixture shape: the repo really does have a detectable origin/HEAD.
  REQUIRE(sh(std::format("git -C '{}' symbolic-ref --short refs/remotes/origin/HEAD", repo)));

  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "git-arms", "active");
  auto const      task_id = seed_task(conn, plan_id, "git-task", "done");

  seed_claim(conn, "arm-merged", task_id, "released", k_future, repo, "merged-branch", base_sha);
  seed_claim(conn, "arm-unmerged", task_id, "released", k_future, repo, "unmerged-branch", base_sha);
  seed_claim(conn, "arm-absent", task_id, "released", k_future, repo, "absent-branch", base_sha);
  seed_claim(conn, "arm-nosha", task_id, "released", k_future, repo, "merged-branch", std::nullopt);
  seed_claim(conn, "arm-longname", task_id, "released", k_future, repo, long_branch, base_sha);

  auto const result = co::evaluate(conn, plan_id, false, true);
  REQUIRE(result.has_value());
  REQUIRE(result->git.size() == 5);

  // Every entry detected the target, which is the property the oracle cannot
  // reach at all.
  for (auto const& entry : result->git) {
    INFO("entry branch: " << entry.branch.value_or("<null>"));
    REQUIRE(entry.target_branch.has_value());
    CHECK(*entry.target_branch == "main");
  }

  CHECK(result->git[0].base_merged == true);
  CHECK(result->git[0].branch_merged == true);
  CHECK(result->git[0].note == "base-merged=true (weak signal); branch-merged=true");

  CHECK(result->git[1].base_merged == true);
  CHECK(result->git[1].branch_merged == false);
  CHECK(result->git[1].note == "base-merged=true (weak signal); branch-merged=false");

  // An absent branch REPLACES the composed note rather than joining it —
  // note `base_merged` is still reported in the FIELD even though the note
  // does not mention it.
  CHECK(result->git[2].base_merged == true);
  CHECK_FALSE(result->git[2].branch_merged.has_value());
  CHECK(result->git[2].note == "branch absent — inconclusive");

  // The `base-merged=unknown` arm: a branch answered, a sha did not.
  CHECK_FALSE(result->git[3].base_merged.has_value());
  CHECK(result->git[3].branch_merged == true);
  CHECK(result->git[3].note == "base-merged=unknown; branch-merged=true");

  // TASK 6321. A 120-byte branch name -- `refs/heads/` + 120 = 131, past the
  // removed 128-byte ceiling. It must be PROBED, not declared absent. The
  // discriminating assertion is `branch_merged.has_value()`: before the fix
  // this arm produced "branch absent -- inconclusive" with no value, exactly
  // like `arm-absent` above, for a branch that exists.
  CHECK(result->git[4].branch_merged.has_value());
  CHECK(result->git[4].branch_merged == false); // real answer: created off main, never merged back
  CHECK(result->git[4].note != "branch absent — inconclusive");

  REQUIRE(result->epic_merge.has_value());
  CHECK(result->epic_merge->target_branch == "main");
  CHECK(result->epic_merge->total_branches == 4); // merged, unmerged, absent, long-name
  CHECK(result->epic_merge->merged_count == 1);   // only `merged-branch`
  CHECK(result->epic_merge->note == "1 of 4 contributing branch(es) merged to main (advisory; absent branches inconclusive)");

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_CASE("a repository git cannot read reports unavailable rather than guessing", "[engine][planning][closeout][git]") {
  // The paired NEGATIVE for the case above: when detection genuinely fails
  // the entry must be the all-null `git-evidence unavailable` shape — which
  // is also, not coincidentally, the only shape the oracle ever produces.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = seed_plan(conn, "no-repo", "active");
  auto const      task_id = seed_task(conn, plan_id, "no-repo-task", "done");
  seed_claim(conn, "nowhere", task_id, "released", k_future, "/nonexistent/co6317/definitely-not-a-repo", "whatever", "abcdef");

  auto const result = co::evaluate(conn, plan_id, false, true);
  REQUIRE(result.has_value());
  REQUIRE(result->git.size() == 1);
  CHECK_FALSE(result->git[0].target_branch.has_value());
  CHECK_FALSE(result->git[0].base_merged.has_value());
  CHECK_FALSE(result->git[0].branch_merged.has_value());
  CHECK(result->git[0].note == "git-evidence unavailable");

  REQUIRE(result->epic_merge.has_value());
  CHECK(result->epic_merge->target_branch == "(unknown)");
  CHECK(result->epic_merge->merged_count == 0);
  CHECK(result->epic_merge->note == "git-evidence unavailable — epic-merge check inconclusive");
}
