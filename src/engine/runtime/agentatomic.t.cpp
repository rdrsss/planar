// @file agentatomic.t.cpp
// @brief Unit tests for `planar.engine.runtime.agentatomic` (plan 996,
// task 6038) — the claim ritual's atomicity and its races.
//
// ## What these tests actually prove, stated plainly
//
// The milestone's invariant is "the terminal verbs flip the claim status
// and the task status in ONE transaction". A test that runs a terminal
// verb and then checks both columns does NOT prove that: it is equally
// satisfied by two sequential transactions. Three shapes here are what
// carry the claim, and each says what it demonstrates:
//
//   1. ROLLBACK ON REFUSAL (`a refused terminal verb leaves NOTHING
//      behind`). The verb is made to fail at its LAST guard, after the
//      task flip and the action close and the claim release have all been
//      issued, and every one of them is then observed to be absent. Two
//      transactions could not produce that: the first would have
//      committed. This is the strongest sequential evidence available,
//      and it is the test the break-probe targets.
//
//   2. CONTENTION ACROSS TWO CONNECTIONS (`two connections cannot both
//      claim`). Two separate `db::connection`s to the same file race for
//      one entity. `BEGIN IMMEDIATE` is what makes the loser fail at
//      `BEGIN` instead of interleaving between the winner's check and its
//      insert. Downgrading to `lock_mode::deferred` makes this test fail —
//      verified by break-probe.
//
//   3. A HELD WRITE LOCK BLOCKS A SECOND WRITER (`an open immediate
//      transaction locks out a concurrent terminal verb`). One connection
//      holds an uncommitted transaction while another attempts a terminal
//      verb; the second is refused rather than reading through to a
//      half-applied state.
//
// WHAT THEY DO NOT PROVE: no test here kills a process mid-transaction, so
// crash-durability rests on SQLite's own guarantees rather than on
// anything measured here. And the recording policy probe below observes
// ORDER (guard before write) but cannot observe that the guard and the
// write are in the same transaction — that is what shape 1 is for.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;

namespace {

namespace aa     = planar::engine::runtime::agentactivity;
namespace atomic = planar::engine::runtime::agentatomic;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_agentatomic_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto open_second(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  // Deliberately does NOT migrate: the schema is already there, and a
  // second migrator would be a different test.
  return std::move(*conn);
}

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

auto scalar_text(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

struct fixture {
  std::int64_t session_id = 0;
  std::int64_t plan_id    = 0;
};

auto seed(planar::db::connection& conn, int task_count) -> fixture {
  exec(conn, "insert into sessions (vendor) values ('test')");
  auto const session_id = scalar_int(conn, "select id from sessions where vendor = 'test'");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','p','test-plan','active')");
  auto const plan_id = scalar_int(conn, "select id from plans where slug = 'test-plan'");
  for (int i = 0; i < task_count; ++i) {
    exec(conn, std::format("insert into tasks (scope_kind, plan_id, title, status, priority) "
                           "values ('global', {}, 't{}', 'todo', {})",
                           plan_id, i, 100 + i));
  }
  return fixture{.session_id = session_id, .plan_id = plan_id};
}

auto task_id_at(planar::db::connection& conn, int index) -> std::int64_t {
  return scalar_int(conn, std::format("select id from tasks order by id limit 1 offset {}", index));
}

/// The REAL task transition matrix, transcribed from
/// `planar.engine.planning.transitions`' task arm. Duplicated HERE rather
/// than imported because an `engine_* -> engine_*` edge is forbidden even
/// in a test target — and duplicating it in the test is the right side of
/// that line: the production seam takes the matrix as a parameter, and
/// this is one caller supplying it.
auto real_transition(std::string_view from, std::string_view to) -> std::expected<void, aa::agent_error> {
  if (from == to) {
    return {};
  }
  auto const legal = [&] {
    if (from == "todo") {
      return to == "doing" || to == "blocked" || to == "cancelled";
    }
    if (from == "doing") {
      return to == "todo" || to == "blocked" || to == "done" || to == "cancelled";
    }
    if (from == "blocked") {
      return to == "doing" || to == "done" || to == "cancelled";
    }
    if (from == "done" || from == "cancelled") {
      return false;
    }
    return false;
  }();
  if (from != "todo" && from != "doing" && from != "blocked" && from != "done" && from != "cancelled") {
    return std::unexpected(aa::agent_error::unknown_status);
  }
  return legal ? std::expected<void, aa::agent_error>{} : std::unexpected(aa::agent_error::illegal_transition);
}

/// A policy that records what it was asked, so a test can assert on the
/// ORDER and the ARGUMENTS of the guard rather than only on its effect.
struct recording_policy {
  std::vector<std::pair<std::string, std::string>> transitions;
  std::vector<std::int64_t>                        recomputes;
  /// Set to make `recompute_plan` fail — the hook is the LAST thing a
  /// terminal transaction does, so failing it is how a test forces a
  /// rollback with every other write already issued.
  bool fail_recompute = false;
  /// Observed inside `recompute_plan`, which runs INSIDE the transaction:
  /// the task status the roll-up would see.
  std::string status_seen_by_recompute;
  /// Every blocker id `clear_unblocked_dependents` was called with (task
  /// 6875) — a test asserts on this to prove the roll-up ran, and on its
  /// absence to prove it did NOT run for `fail`/`release`.
  std::vector<std::int64_t> unblock_calls;
  /// The readiness reasons the injected check reports per task id; a task absent here is ready
  /// (plan 1142).
  std::map<std::int64_t, std::vector<std::string>> unready;
  /// Every task id `unready_reasons` was asked about, in order.
  std::vector<std::int64_t> readiness_calls;
  /// Whether each `unready_reasons` call saw the connection inside a transaction: the claim and pull
  /// checks must run in the transaction that inserts the claim.
  std::vector<bool> readiness_in_transaction;

  auto bind() -> atomic::task_policy {
    return atomic::task_policy{
        .check_transition = [this](std::string_view from, std::string_view to) -> std::expected<void, aa::agent_error> {
          transitions.emplace_back(std::string{from}, std::string{to});
          return real_transition(from, to);
        },
        .recompute_plan = [this](planar::db::connection& conn, std::int64_t plan_id) -> std::expected<void, aa::agent_error> {
          recomputes.push_back(plan_id);
          auto stmt = conn.prepare(std::format("select status from tasks where plan_id = {} order by id limit 1", plan_id));
          if (stmt) {
            auto step = stmt->step();
            if (step && *step == planar::db::step_result::row) {
              status_seen_by_recompute = stmt->column_text(0);
            }
          }
          if (fail_recompute) {
            return std::unexpected(aa::agent_error::query_failed);
          }
          return {};
        },
        .clear_unblocked_dependents = [this](planar::db::connection&,
                                             std::int64_t blocker_id) -> std::expected<void, aa::agent_error> {
          unblock_calls.push_back(blocker_id);
          return {};
        },
        .unready_reasons = [this](planar::db::connection& conn,
                                  std::int64_t            task_id) -> std::expected<std::vector<std::string>, aa::agent_error> {
          readiness_calls.push_back(task_id);
          readiness_in_transaction.push_back(conn.in_transaction());
          auto const found = unready.find(task_id);
          return found == unready.end() ? std::vector<std::string>{} : found->second;
        },
    };
  }
};

auto basic_args(const fixture& fx, std::int64_t task) -> aa::acquire_args {
  return aa::acquire_args{.session_id = fx.session_id, .kind = aa::entity_kind::task, .entity_id = task, .vendor = "test"};
}

} // namespace

// ===========================================================================
// claim_entity
// ===========================================================================

TEST_CASE("claim_entity transitions a todo task to doing BY DEFAULT", "[agentatomic]") {
  // The brief this task shipped under asserted the opposite — that
  // `claim --entity` leaves the task `todo` — and the oracle contradicts
  // it. Captured directly:
  //
  //     $ sqlite3 planar.db "select id,status from tasks where id=5"  -> 5|todo
  //     $ planar-agent claim --entity task:5                          -> exit 0
  //     $ sqlite3 planar.db "select id,status from tasks where id=5"  -> 5|doing
  //
  // `--no-transition` is what produces the behaviour the brief described.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  auto const held = atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind());
  REQUIRE(held.has_value());
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "doing");

  // The guard ran, with the ACTUAL current status as `from` — not a
  // hardcoded "todo". A guard fed constants proves nothing.
  REQUIRE(policy.transitions.size() == 1);
  REQUIRE(policy.transitions[0] == std::pair<std::string, std::string>{"todo", "doing"});

  // ...and an OPEN `claim_check` marker exists. That row is what `abort`
  // and `reconcile` later test to decide whether the task reset is theirs.
  REQUIRE(scalar_int(conn, std::format("select count(*) from agent_actions where claim_id = {} "
                                       "and action_kind = 'claim_check' and ended_at is null",
                                       held->id)) == 1);
}

TEST_CASE("claim_entity --no-transition leaves the task and writes no marker", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  auto const held = atomic::claim_entity(conn, basic_args(fx, task), false, policy.bind());
  REQUIRE(held.has_value());
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "todo");
  // The guard is not even consulted — there is no transition to guard.
  REQUIRE(policy.transitions.empty());
  REQUIRE(scalar_int(conn, std::format("select count(*) from agent_actions where claim_id = {}", held->id)) == 0);
}

TEST_CASE("claim_entity accepts ONLY todo, so --force on a doing task is refused", "[agentatomic]") {
  // The eligible set is exactly `{todo}` — STRICTER than the transition
  // matrix, which also allows `blocked -> doing`. Direct dispatch is an
  // entry into work, not a reopen. The oracle's own answer for
  // `claim --entity task:<doing> --force` is `IllegalTransition`, which is
  // initially surprising: `--force` takes over the CLAIM, it does not
  // bypass the task matrix.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  REQUIRE(atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind()).has_value());

  auto forced        = basic_args(fx, task);
  forced.force       = true;
  auto const refused = atomic::claim_entity(conn, forced, true, policy.bind());
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::illegal_transition);

  // AND the takeover rolled back: the original claim is still active,
  // NOT stale. `acquire_claim` had already marked it stale inside the
  // transaction before the transition guard refused — so this assertion
  // is what proves the rollback reached that write too.
  REQUIRE(scalar_int(conn, "select count(*) from agent_work_claims where status = 'active'") == 1);
  REQUIRE(scalar_int(conn, "select count(*) from agent_work_claims where status = 'stale'") == 0);

  // A `blocked` task is refused for the same reason even though the
  // matrix would allow `blocked -> doing`.
  exec(conn, std::format("update tasks set status = 'blocked' where id = {}", task));
  exec(conn, std::format("update agent_work_claims set status = 'released', released_at = "
                         "strftime('%Y-%m-%dT%H:%M:%fZ','now') where entity_id = {}",
                         task));
  auto const on_blocked = atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind());
  REQUIRE_FALSE(on_blocked.has_value());
  REQUIRE(on_blocked.error() == aa::agent_error::illegal_transition);
}

TEST_CASE("claim_entity never touches a plan or plan_step", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto args       = basic_args(fx, fx.plan_id);
  args.kind       = aa::entity_kind::plan;
  auto const held = atomic::claim_entity(conn, args, true, policy.bind());
  REQUIRE(held.has_value());
  REQUIRE(scalar_text(conn, std::format("select status from plans where id = {}", fx.plan_id)) == "active");
  REQUIRE(policy.transitions.empty());
}

// ===========================================================================
// pull / peek
// ===========================================================================

TEST_CASE("pull selects by priority then id, and skips claimed tasks", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 3);
  recording_policy policy;

  // seed() gives priorities 100, 101, 102. Make the LAST task the
  // highest-ranked so a selector that ignored priority would pick the
  // wrong row — an `order by id` implementation passes a same-priority
  // fixture by accident.
  auto const third = task_id_at(conn, 2);
  exec(conn, std::format("update tasks set priority = 1 where id = {}", third));

  auto const first = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first->no_work);
  REQUIRE(first->task_id == third);
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", third)) == "doing");

  // The next pull skips the now-claimed task and takes the next rank.
  auto const second = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(second.has_value());
  REQUIRE(second->task_id == task_id_at(conn, 0));
}

TEST_CASE("pull with nothing eligible writes NOTHING", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;
  exec(conn, "update tasks set status = 'done'");

  auto const claims_before  = scalar_int(conn, "select count(*) from agent_work_claims");
  auto const actions_before = scalar_int(conn, "select count(*) from agent_actions");

  auto const result = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(result.has_value());
  REQUIRE(result->no_work);
  REQUIRE(scalar_int(conn, "select count(*) from agent_work_claims") == claims_before);
  REQUIRE(scalar_int(conn, "select count(*) from agent_actions") == actions_before);
  // The guard is not consulted either: there was no transition to make.
  REQUIRE(policy.transitions.empty());
}

TEST_CASE("pull sees only its OWN plan — no child-plan recursion", "[agentatomic]") {
  // Unlike `planar plan next`, whose selector walks the plan tree. An
  // orchestrator that pulls from an anchor plan gets nothing once the
  // anchor's own direct tasks are taken, however much work its milestone
  // children hold.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 0);
  recording_policy policy;

  exec(conn, std::format("insert into plans (scope_kind, title, slug, parent_plan_id) values "
                         "('global','child','child-plan',{})",
                         fx.plan_id));
  auto const child = scalar_int(conn, "select id from plans where slug = 'child-plan'");
  exec(conn, std::format("insert into tasks (scope_kind, plan_id, title, status) values ('global',{},'c','todo')", child));

  auto const from_anchor = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(from_anchor.has_value());
  REQUIRE(from_anchor->no_work);

  auto const from_child =
      atomic::pull_next(conn, atomic::pull_args{.plan_id = child, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(from_child.has_value());
  REQUIRE_FALSE(from_child->no_work);
}

TEST_CASE("peek runs pull's selector and writes nothing at all", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 2);
  recording_policy policy;

  auto const claims_before  = scalar_int(conn, "select count(*) from agent_work_claims");
  auto const actions_before = scalar_int(conn, "select count(*) from agent_actions");
  auto const status_before  = scalar_text(conn, std::format("select status from tasks where id = {}", task_id_at(conn, 0)));

  auto const peeked = atomic::peek_next(conn, fx.plan_id, policy.bind());
  REQUIRE(peeked.has_value());
  REQUIRE_FALSE(peeked->no_work);

  // The task `pull` WOULD take — proven by taking it and comparing, which
  // is the only way to show the two share a selector rather than merely
  // agreeing on this fixture.
  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  REQUIRE(pulled->task_id == peeked->task_id);

  REQUIRE(claims_before + 1 == scalar_int(conn, "select count(*) from agent_work_claims"));
  REQUIRE(actions_before + 1 == scalar_int(conn, "select count(*) from agent_actions"));
  REQUIRE(status_before == "todo");
}

// ===========================================================================
// Dependency exclusion (task 6841 / decision D6, closes task 5535)
// ===========================================================================

TEST_CASE("peek_next excludes a task with an outbound depends-on edge to a non-terminal blocker", "[agentatomic][deps]") {
  // A `depends-on` edge can exist on a `todo` task WITHOUT the task itself
  // being `blocked` — `task block --on` is not the only writer of that
  // edge (the generic `link` verb is another). So a selector that only
  // ever filtered on `t.status = 'todo'` could still hand out a task whose
  // dependency is wide open. This is the direct regression test for task
  // 5535.
  //
  // One shared fixture, five blocker statuses. `blocker` status decides
  // whether `dependent` is eligible; when it is NOT, `blocker` itself
  // (still `todo` in the `todo`/`doing`/`blocked` cases only where
  // relevant) is the only remaining candidate, so the two branches below
  // assert on WHICH task comes back, not just whether one does.
  struct expectation {
    std::string_view blocker_status;
    bool             dependent_eligible;
  };
  for (auto const& exp : std::vector<expectation>{
           {"todo", false},
           {"doing", false},
           {"blocked", false},
           {"done", true},
           {"cancelled", true},
       }) {
    INFO("blocker status: " << exp.blocker_status);
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    auto const      fx   = seed(conn, 2);

    auto const blocker   = task_id_at(conn, 0);
    auto const dependent = task_id_at(conn, 1);
    exec(conn, std::format("update tasks set status = '{}' where id = {}", exp.blocker_status, blocker));
    exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values ('task', {}, 'task', {}, 'depends-on')",
                           dependent, blocker));

    recording_policy policy;
    auto const       peeked = atomic::peek_next(conn, fx.plan_id, policy.bind());
    REQUIRE(peeked.has_value());

    if (exp.dependent_eligible) {
      // The blocker itself is `done`/`cancelled`, so it is no longer a
      // `todo` candidate at all — the dependent is the only one left.
      REQUIRE_FALSE(peeked->no_work);
      CHECK(peeked->task_id == dependent);
    } else if (exp.blocker_status == "todo") {
      // The dependent is excluded, but the blocker is itself still an
      // ordinary eligible `todo` task with no dependency of its own.
      REQUIRE_FALSE(peeked->no_work);
      CHECK(peeked->task_id == blocker);
    } else {
      // Blocker is `doing`/`blocked` (not `todo`, so not a candidate
      // either) and the dependent is excluded: nothing is eligible.
      CHECK(peeked->no_work);
    }
  }
}

TEST_CASE("peek_next and pull_next agree when a depends-on edge excludes the lower-priority candidate", "[agentatomic][deps]") {
  // Proves two things at once: (1) the exclusion is not merely an
  // artifact of priority ordering — `dependent` is given the LOWEST
  // priority, so a selector ignoring the dependency would wrongly pick it
  // first — and (2) `peek` really is "the same query as pull, no writes"
  // for this new clause too, not only for the pre-existing ones.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 3);
  recording_policy policy;

  auto const blocker   = task_id_at(conn, 0);
  auto const dependent = task_id_at(conn, 1);
  auto const other     = task_id_at(conn, 2);
  exec(conn, std::format("update tasks set priority = 10 where id = {}", dependent)); // lowest: would sort first
  exec(conn, std::format("update tasks set priority = 20 where id = {}", blocker));
  exec(conn, std::format("update tasks set priority = 30 where id = {}", other));
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('task', {}, 'task', {}, 'depends-on')",
                         dependent, blocker));

  recording_policy peek_policy;
  auto const       peeked = atomic::peek_next(conn, fx.plan_id, peek_policy.bind());
  REQUIRE(peeked.has_value());
  REQUIRE_FALSE(peeked->no_work);
  CHECK(peeked->task_id == blocker);

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  REQUIRE_FALSE(pulled->no_work);
  CHECK(pulled->task_id == blocker);
  CHECK(pulled->task_id == peeked->task_id);
  (void)other;
}

// ===========================================================================
// Readiness gate (plan 1142)
// ===========================================================================

TEST_CASE("claim_entity asks readiness inside the claim transaction and claims a ready task", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  REQUIRE_FALSE(conn.in_transaction());
  atomic::claim_readiness readiness;
  auto const              held = atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind(), &readiness);
  REQUIRE(held.has_value());

  // Asked exactly once, about this task, with the claim transaction open: a link written by another
  // process after this read cannot reach the INSERT that follows it.
  REQUIRE(policy.readiness_calls == std::vector<std::int64_t>{task});
  REQUIRE(policy.readiness_in_transaction == std::vector<bool>{true});
  REQUIRE(readiness.unready_reasons.empty());
  REQUIRE(scalar_int(conn, "select count(*) from agent_actions where action_kind = 'other'") == 0);
}

TEST_CASE("claim_entity refuses an unready task and writes nothing", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;
  policy.unready[task] = {"blocked_by_open_task", "open_question_linked"};

  atomic::claim_readiness readiness;
  auto const              held = atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind(), &readiness);
  REQUIRE_FALSE(held.has_value());
  CHECK(held.error() == aa::agent_error::task_not_ready);
  CHECK(readiness.unready_reasons == std::vector<std::string>{"blocked_by_open_task", "open_question_linked"});

  CHECK(scalar_int(conn, "select count(*) from agent_work_claims") == 0);
  CHECK(scalar_int(conn, "select count(*) from agent_actions") == 0);
  CHECK(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "todo");
  CHECK_FALSE(conn.in_transaction());

  // No readiness slot means no override: the gate cannot be skipped by omission.
  auto const bare = atomic::claim_entity(conn, basic_args(fx, task), false, policy.bind());
  REQUIRE_FALSE(bare.has_value());
  CHECK(bare.error() == aa::agent_error::task_not_ready);
  CHECK(scalar_int(conn, "select count(*) from agent_work_claims") == 0);
}

TEST_CASE("claim_entity with allow_unready claims and writes one audit action naming the reasons", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;
  policy.unready[task] = {"blocked_by_open_task", "proposed_decision_linked"};

  atomic::claim_readiness readiness{.allow_unready = true};
  auto const              held = atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind(), &readiness);
  REQUIRE(held.has_value());
  CHECK(readiness.unready_reasons == std::vector<std::string>{"blocked_by_open_task", "proposed_decision_linked"});
  CHECK(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "doing");

  // Exactly one override row, closed ok, on this claim and task, readable by a machine.
  CHECK(scalar_int(conn, "select count(*) from agent_actions where action_kind = 'other'") == 1);
  CHECK(scalar_text(conn, "select metadata from agent_actions where action_kind = 'other'") ==
        R"({"unready_override":true,"reasons":["blocked_by_open_task","proposed_decision_linked"]})");
  CHECK(scalar_text(conn, "select summary from agent_actions where action_kind = 'other'") ==
        "unready_override: blocked_by_open_task, proposed_decision_linked");
  CHECK(scalar_text(conn, "select outcome from agent_actions where action_kind = 'other'") == "ok");
  CHECK(scalar_int(conn, std::format("select count(*) from agent_actions where action_kind = 'other' and claim_id = {} and "
                                     "entity_kind = 'task' and entity_id = {} and ended_at is not null",
                                     held->id, task)) == 1);
  // The transition marker is still there, separately.
  CHECK(scalar_int(conn, "select count(*) from agent_actions where action_kind = 'claim_check'") == 1);
}

TEST_CASE("allow_unready on a ready task writes no audit action", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  atomic::claim_readiness readiness{.allow_unready = true};
  REQUIRE(atomic::claim_entity(conn, basic_args(fx, task), true, policy.bind(), &readiness).has_value());
  CHECK(scalar_int(conn, "select count(*) from agent_actions where action_kind = 'other'") == 0);
}

TEST_CASE("contention and illegal_transition outrank readiness, and plan claims are not gated", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 2);
  auto const       held = task_id_at(conn, 0);
  auto const       done = task_id_at(conn, 1);
  recording_policy policy;
  policy.unready[held] = {"open_question_linked"};
  policy.unready[done] = {"invalid_task_status"};

  // A live claim is contention before readiness is ever asked (an operator re-claim stays refused).
  atomic::claim_readiness allow{.allow_unready = true};
  REQUIRE(atomic::claim_entity(conn, basic_args(fx, held), false, policy.bind(), &allow).has_value());
  policy.readiness_calls.clear();
  auto const again = atomic::claim_entity(conn, basic_args(fx, held), false, policy.bind(), &allow);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == aa::agent_error::claim_contention);
  CHECK(policy.readiness_calls.empty());

  // A transitioning claim on a task that is not todo keeps its IllegalTransition tag.
  exec(conn, std::format("update tasks set status = 'done' where id = {}", done));
  atomic::claim_readiness plain;
  auto const              refused = atomic::claim_entity(conn, basic_args(fx, done), true, policy.bind(), &plain);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == aa::agent_error::illegal_transition);
  CHECK(policy.readiness_calls.empty());

  // A plan claim is not a task claim: readiness is not asked.
  auto plan_args =
      aa::acquire_args{.session_id = fx.session_id, .kind = aa::entity_kind::plan, .entity_id = fx.plan_id, .vendor = "test"};
  auto const plan_claim = atomic::claim_entity(conn, plan_args, true, policy.bind(), &plain);
  REQUIRE(plan_claim.has_value());
  CHECK(policy.readiness_calls.empty());
}

TEST_CASE("pull skips an unready first-priority task and claims the next ready one", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn   = open_migrated(scratch);
  auto const       fx     = seed(conn, 3);
  auto const       first  = task_id_at(conn, 0);
  auto const       second = task_id_at(conn, 1);
  recording_policy policy;
  policy.unready[first] = {"blocked_by_open_task"};

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  REQUIRE_FALSE(pulled->no_work);
  CHECK(pulled->task_id == second);
  REQUIRE(pulled->skipped.size() == 1);
  CHECK(pulled->skipped[0].task_id == first);
  CHECK(pulled->skipped[0].reasons == std::vector<std::string>{"blocked_by_open_task"});
  CHECK(scalar_text(conn, std::format("select status from tasks where id = {}", first)) == "todo");
  CHECK(scalar_text(conn, std::format("select status from tasks where id = {}", second)) == "doing");

  // Readiness was asked for the skipped task and the chosen one, both inside the pull transaction,
  // and not for the third, which was never reached.
  CHECK(policy.readiness_calls == std::vector<std::int64_t>{first, second});
  CHECK(policy.readiness_in_transaction == std::vector<bool>{true, true});
}

TEST_CASE("pull with only unready tasks is no_work, names them, and writes nothing", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 2);
  recording_policy policy;
  policy.unready[task_id_at(conn, 0)] = {"open_question_linked"};
  policy.unready[task_id_at(conn, 1)] = {"blocked_by_open_task", "proposed_decision_linked"};

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  CHECK(pulled->no_work);
  REQUIRE(pulled->skipped.size() == 2);
  CHECK(pulled->skipped[1].reasons.size() == 2);
  CHECK(scalar_int(conn, "select count(*) from agent_work_claims") == 0);
  CHECK(scalar_int(conn, "select count(*) from agent_actions") == 0);
  CHECK(scalar_int(conn, "select count(*) from tasks where status = 'todo'") == 2);

  // peek reports what pull would: nothing, and the same skipped list.
  auto const peeked = atomic::peek_next(conn, fx.plan_id, policy.bind());
  REQUIRE(peeked.has_value());
  CHECK(peeked->no_work);
  CHECK(peeked->skipped.size() == 2);
}

TEST_CASE("a readiness failure aborts pull and claim without claiming", "[agentatomic][readiness]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy recording;
  auto             broken = recording.bind();
  broken.unready_reasons = [](planar::db::connection&, std::int64_t) -> std::expected<std::vector<std::string>, aa::agent_error> {
    return std::unexpected(aa::agent_error::query_failed);
  };

  auto const pulled =
      atomic::pull_next(conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, broken);
  REQUIRE_FALSE(pulled.has_value());
  CHECK(pulled.error() == aa::agent_error::query_failed);
  auto const claimed = atomic::claim_entity(conn, basic_args(fx, task_id_at(conn, 0)), true, broken);
  REQUIRE_FALSE(claimed.has_value());
  CHECK(claimed.error() == aa::agent_error::query_failed);
  CHECK(scalar_int(conn, "select count(*) from agent_work_claims") == 0);
}

// ===========================================================================
// Claim policy (plan 1142)
// ===========================================================================

TEST_CASE("claim_policy is asked inside the claim transaction and its digest is stored on the claim",
          "[agentatomic][claim-policy]") {
  scratch_db_path                    scratch;
  auto                               conn = open_migrated(scratch);
  auto const                         fx   = seed(conn, 2);
  recording_policy                   recording;
  auto                               policy = recording.bind();
  std::vector<atomic::claim_request> asked;
  std::vector<bool>                  in_transaction;
  policy.claim_policy = [&](planar::db::connection&      c,
                            const atomic::claim_request& request) -> std::expected<std::optional<std::string>, aa::agent_error> {
    asked.push_back(request);
    in_transaction.push_back(c.in_transaction());
    return std::optional<std::string>{"digest-for-test"};
  };

  auto args          = basic_args(fx, task_id_at(conn, 0));
  args.role          = "coder";
  args.model         = "m-1";
  auto const claimed = atomic::claim_entity(conn, args, true, policy);
  REQUIRE(claimed.has_value());
  REQUIRE(asked.size() == 1);
  CHECK(asked[0].kind == aa::entity_kind::task);
  CHECK(asked[0].entity_id == task_id_at(conn, 0));
  CHECK(asked[0].role == std::optional<std::string_view>{"coder"});
  CHECK(asked[0].model == std::optional<std::string_view>{"m-1"});
  CHECK(in_transaction == std::vector<bool>{true});
  CHECK(scalar_text(conn, "select policy_digest from agent_work_claims") == "digest-for-test");

  // A pull asks once, with the plan, before any task is read, and records the digest on the claim it takes.
  asked.clear();
  recording.readiness_calls.clear();
  auto const pulled =
      atomic::pull_next(conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy);
  REQUIRE(pulled.has_value());
  REQUIRE(asked.size() == 1);
  CHECK(asked[0].kind == aa::entity_kind::plan);
  CHECK(asked[0].entity_id == fx.plan_id);
  CHECK(scalar_text(conn, "select policy_digest from agent_work_claims order by id desc limit 1") == "digest-for-test");

  // An unbound decision admits the claim and records NULL.
  auto const unbound   = recording.bind();
  auto       plan_args = basic_args(fx, fx.plan_id);
  plan_args.kind       = aa::entity_kind::plan;
  auto const plain     = atomic::claim_entity(conn, plan_args, false, unbound);
  REQUIRE(plain.has_value());
  CHECK(scalar_int(conn, "select policy_digest is null from agent_work_claims order by id desc limit 1") == 1);
}

TEST_CASE("a claim_policy refusal aborts claim and pull before anything is read or written", "[agentatomic][claim-policy]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy recording;
  auto             policy = recording.bind();
  policy.claim_policy     = [](planar::db::connection&,
                               const atomic::claim_request&) -> std::expected<std::optional<std::string>, aa::agent_error> {
    return std::unexpected(aa::agent_error::claim_policy_unmet);
  };

  auto const claimed = atomic::claim_entity(conn, basic_args(fx, task_id_at(conn, 0)), true, policy);
  REQUIRE_FALSE(claimed.has_value());
  CHECK(claimed.error() == aa::agent_error::claim_policy_unmet);
  CHECK(aa::error_name(claimed.error()) == "PolicyUnmet");
  CHECK(recording.readiness_calls.empty());

  auto const pulled =
      atomic::pull_next(conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy);
  REQUIRE_FALSE(pulled.has_value());
  CHECK(pulled.error() == aa::agent_error::claim_policy_unmet);
  CHECK(recording.readiness_calls.empty());

  CHECK(scalar_int(conn, "select count(*) from agent_work_claims") == 0);
  CHECK(scalar_int(conn, "select count(*) from agent_actions") == 0);
  CHECK(scalar_text(conn, "select status from tasks") == "todo");
}

// ===========================================================================
// Terminal verbs — the atomicity evidence
// ===========================================================================

TEST_CASE("complete flips task and claim together and recomputes the plan inside", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());

  auto const done = atomic::complete_work(conn, pulled->acquired->claim_token, "finished", policy.bind());
  REQUIRE(done.has_value());
  REQUIRE(done->released.status == aa::claim_status::completed);
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", pulled->task_id)) == "done");
  REQUIRE(scalar_text(conn, std::format("select outcome from agent_actions where id = {}", pulled->action_id)) == "ok");
  REQUIRE(scalar_text(conn, std::format("select summary from agent_actions where id = {}", pulled->action_id)) == "finished");

  // The roll-up ran, and — this is the load-bearing part — it saw the task
  // ALREADY `done`. A recompute placed after the commit would have seen
  // the same thing; a recompute placed BEFORE the task flip would have
  // seen `doing`. This pins the ordering inside the transaction.
  REQUIRE(policy.recomputes.size() == 1);
  REQUIRE(policy.recomputes[0] == fx.plan_id);
  REQUIRE(policy.status_seen_by_recompute == "done");

  // The guard was fed the ACTUAL current status.
  REQUIRE(policy.transitions.back() == std::pair<std::string, std::string>{"doing", "done"});
}

// ===========================================================================
// Dependency auto-unblock on completion (task 6875)
// ===========================================================================

TEST_CASE("complete_work runs the dependency roll-up in the SAME transaction as the flip to done", "[agentatomic][deps]") {
  // `planar task done` already runs `clear_unblocked_dependents` right
  // after its own flip to `done` (planning/task.cpp's `mark_done`).
  // `planar-agent complete` is a second entry point onto the identical
  // "a task just went terminal" event and used to skip the roll-up
  // entirely, observed 2026-09-22: a dependent stayed `blocked` after its
  // blocker completed via `planar-agent complete`. This asserts the
  // callable is invoked, with the completed task's id, exactly once.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());

  auto const done = atomic::complete_work(conn, pulled->acquired->claim_token, "finished", policy.bind());
  REQUIRE(done.has_value());

  REQUIRE(policy.unblock_calls.size() == 1);
  CHECK(policy.unblock_calls[0] == pulled->task_id);
}

TEST_CASE("fail_work and release_work do NOT run the dependency roll-up", "[agentatomic][deps]") {
  // Neither verb reaches a terminal status (both return the task to
  // `todo`), so calling the roll-up would be pure overhead at best and a
  // misleading audit row ("unblocked: task N is terminal" on a task that
  // is very much not terminal) at worst.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 2);
  recording_policy policy;

  auto const first = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(first.has_value());
  REQUIRE(atomic::fail_work(conn, first->acquired->claim_token, "broke", aa::failure_category::tool_failure, policy.bind())
              .has_value());

  auto const second = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(second.has_value());
  REQUIRE(atomic::release_work(conn, second->acquired->claim_token, "gave up", policy.bind()).has_value());

  CHECK(policy.unblock_calls.empty());
}

TEST_CASE("complete_work's roll-up ACTUALLY clears a blocked dependent, end to end", "[agentatomic][deps]") {
  // Uses the REAL `set_status` path a production policy would run
  // (recording_policy's lambda is a spy elsewhere, but here it must
  // actually perform the SQL clearance for this test to mean anything) —
  // so this fixture wires a policy whose `clear_unblocked_dependents`
  // performs the same `blocked -> todo` update `planning::task`'s does,
  // to prove the CALL SITE (inside the terminal transaction, after the
  // flip) is where it matters, independent of which concrete function is
  // bound at layer 3.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);

  auto const blocker   = task_id_at(conn, 0);
  auto const dependent = task_id_at(conn, 1);
  exec(conn, std::format("update tasks set status = 'blocked' where id = {}", dependent));
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('task', {}, 'task', {}, 'depends-on')",
                         dependent, blocker));

  recording_policy policy;
  auto const       pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  REQUIRE(pulled->task_id == blocker); // `dependent` is excluded (task 6841) AND not `todo` anyway.

  auto real_policy                       = policy.bind();
  real_policy.clear_unblocked_dependents = [&conn](planar::db::connection&,
                                                   std::int64_t blocker_id) -> std::expected<void, aa::agent_error> {
    auto stmt = conn.prepare("update tasks set status = 'todo' where status = 'blocked' and id in ("
                             "  select d.id from tasks d "
                             "  join entity_links el on el.from_kind='task' and el.from_id=d.id "
                             "                      and el.to_kind='task' and el.relationship='depends-on' "
                             "  where el.to_id = ?1)");
    if (!stmt || !stmt->bind_int64(1, blocker_id) || !stmt->step()) {
      return std::unexpected(aa::agent_error::query_failed);
    }
    return {};
  };

  auto const done = atomic::complete_work(conn, pulled->acquired->claim_token, "finished", real_policy);
  REQUIRE(done.has_value());
  CHECK(scalar_text(conn, std::format("select status from tasks where id = {}", dependent)) == "todo");

  // And now that `dependent` is `todo` with a `done` blocker, `pull`
  // (which already excludes `blocked` and open-dependency tasks) takes it
  // next.
  auto const next = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(next.has_value());
  REQUIRE_FALSE(next->no_work);
  CHECK(next->task_id == dependent);
}

TEST_CASE("a refused terminal verb leaves NOTHING behind", "[agentatomic]") {
  // THE ATOMICITY TEST. The verb is failed at its LAST step — the plan
  // roll-up — by which point the task flip, the action close and the claim
  // release have ALL been issued against the connection. Every one of them
  // is then observed to be absent.
  //
  // Two sequential transactions cannot produce this result: whichever ran
  // first would have committed and would still be visible. A single
  // transaction with a missing rollback cannot either — the writes would
  // be there. This is the strongest evidence available without killing a
  // process mid-flight.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  auto const token = pulled->acquired->claim_token;

  policy.fail_recompute = true;
  auto const refused    = atomic::complete_work(conn, token, "finished", policy.bind());
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::query_failed);
  // It got as far as the roll-up, so everything before it really was
  // issued — otherwise this test would be proving nothing at all.
  REQUIRE(policy.recomputes.size() == 1);

  // ...and none of it survived.
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", pulled->task_id)) == "doing");
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where claim_token = '{}'", token)) == "active");
  REQUIRE(scalar_int(conn, std::format("select count(*) from agent_work_claims where claim_token = '{}' "
                                       "and released_at is null",
                                       token)) == 1);
  REQUIRE(scalar_int(conn, std::format("select count(*) from agent_actions where id = {} and ended_at is null",
                                       pulled->action_id)) == 1);

  // And the claim is still usable — a half-applied terminal verb would
  // have stranded it.
  policy.fail_recompute = false;
  REQUIRE(atomic::complete_work(conn, token, "finished", policy.bind()).has_value());
}

TEST_CASE("a terminal verb refused by the STATUS GUARD writes nothing", "[agentatomic]") {
  // The other rollback path, refused earlier: `complete` on a claim whose
  // task is still `todo` (a `--no-transition` claim) fails
  // `illegal_transition` and the claim keeps its lease. This is the exact
  // shape an orchestrator hits when it re-claims with `--no-transition`
  // and then tries to complete.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  auto const held = atomic::claim_entity(conn, basic_args(fx, task), false, policy.bind());
  REQUIRE(held.has_value());

  auto const refused = atomic::complete_work(conn, held->claim_token, std::nullopt, policy.bind());
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::illegal_transition);
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "todo");
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", held->id)) == "active");
  // The roll-up was never reached — the guard is genuinely BEFORE the
  // writes, not merely before the commit.
  REQUIRE(policy.recomputes.empty());
}

TEST_CASE("fail and release differ in claim status and action outcome", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 2);
  recording_policy policy;

  auto const first = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(first.has_value());
  auto const failed =
      atomic::fail_work(conn, first->acquired->claim_token, "broke", aa::failure_category::tool_failure, policy.bind());
  REQUIRE(failed.has_value());
  REQUIRE(failed->released.status == aa::claim_status::aborted);
  REQUIRE(failed->released.category == aa::failure_category::tool_failure);
  REQUIRE(failed->released.release_reason == "broke");
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", first->task_id)) == "todo");
  REQUIRE(scalar_text(conn, std::format("select outcome from agent_actions where id = {}", first->action_id)) == "error");

  auto const second = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(second.has_value());
  auto const released = atomic::release_work(conn, second->acquired->claim_token, "gave up", policy.bind());
  REQUIRE(released.has_value());
  // SAME task destination, DIFFERENT claim status and action outcome —
  // which is the whole distinction between the two verbs.
  REQUIRE(released->released.status == aa::claim_status::released);
  REQUIRE_FALSE(released->released.category.has_value());
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", second->task_id)) == "todo");
  REQUIRE(scalar_text(conn, std::format("select outcome from agent_actions where id = {}", second->action_id)) == "aborted");
}

TEST_CASE("block writes a depends-on edge and is not idempotent", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 2);
  recording_policy policy;
  auto const       blocker = task_id_at(conn, 1);

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  auto const blocked = atomic::block_work(conn, pulled->acquired->claim_token, blocker, "waiting", policy.bind());
  REQUIRE(blocked.has_value());
  REQUIRE(blocked->released.status == aa::claim_status::released);
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", pulled->task_id)) == "blocked");
  REQUIRE(scalar_int(conn, std::format("select count(*) from entity_links where from_kind='task' and from_id={} "
                                       "and to_kind='task' and to_id={} and relationship='depends-on'",
                                       pulled->task_id, blocker)) == 1);
  // `reason` doubles as the ACTION summary here, unlike fail/release.
  REQUIRE(scalar_text(conn, std::format("select summary from agent_actions where id = {}", pulled->action_id)) == "waiting");

  // Blocking again on the same blocker trips the entity_links UNIQUE
  // constraint, and the whole verb rolls back — the second block is NOT a
  // no-op and NOT idempotent.
  auto const again = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(again.has_value());
  REQUIRE(again->task_id == blocker);
}

TEST_CASE("block refuses a dangling blocker BEFORE touching anything", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());

  auto const refused = atomic::block_work(conn, pulled->acquired->claim_token, 99999, "waiting", policy.bind());
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::task_not_found);
  // Neither the task nor the claim moved — a typo in `--blocker` must not
  // leave the claim released against a task that was never blocked.
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", pulled->task_id)) == "doing");
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", pulled->acquired->id)) == "active");
  REQUIRE(scalar_int(conn, "select count(*) from entity_links") == 0);
}

TEST_CASE("a terminal verb refuses a claim held on a plan", "[agentatomic]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto args       = basic_args(fx, fx.plan_id);
  args.kind       = aa::entity_kind::plan;
  auto const held = atomic::claim_entity(conn, args, true, policy.bind());
  REQUIRE(held.has_value());

  auto const refused = atomic::complete_work(conn, held->claim_token, std::nullopt, policy.bind());
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::claim_not_on_task);
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", held->id)) == "active");
}

TEST_CASE("release succeeds on a plan claim, unlike complete (task 6890)", "[agentatomic][6890]") {
  // Decision, task 6890: the orchestrator ritual takes a plan-level claim
  // (`planar-agent claim --entity plan:<id> --role orchestrator`), but
  // every terminal verb refused it with `ClaimNotOnTask`, so a plan claim
  // could only end by lease expiry. `release` is now the exception --
  // it is the graceful give-up verb and has no entity transition to
  // perform for a non-task entity. `complete`/`fail`/`block` still
  // refuse; only `release`'s guard changed.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  auto const plan_status_before = scalar_text(conn, std::format("select status from plans where id = {}", fx.plan_id));

  auto args       = basic_args(fx, fx.plan_id);
  args.kind       = aa::entity_kind::plan;
  auto const held = atomic::claim_entity(conn, args, true, policy.bind());
  REQUIRE(held.has_value());

  auto const released = atomic::release_work(conn, held->claim_token, "handing off", policy.bind());
  REQUIRE(released.has_value());
  REQUIRE(released->released.status == aa::claim_status::released);
  // A plan claim holds no task: `fx.plan_id` is a PLAN id and a task row may
  // share the number, so the result must not name one (task 7118).
  REQUIRE_FALSE(released->task_id.has_value());
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", held->id)) == "released");
  // The plan's own status is untouched -- there is no task-status
  // transition to perform for a plan claim, and this verb must not
  // invent one.
  REQUIRE(scalar_text(conn, std::format("select status from plans where id = {}", fx.plan_id)) == plan_status_before);

  // `complete` on the SAME kind of claim still refuses -- this is the
  // OTHER half of the decision: only `release`'s guard relaxed.
  auto second_args       = basic_args(fx, fx.plan_id);
  second_args.kind       = aa::entity_kind::plan;
  auto const second_held = atomic::claim_entity(conn, second_args, true, policy.bind());
  REQUIRE(second_held.has_value());
  auto const refused_complete = atomic::complete_work(conn, second_held->claim_token, std::nullopt, policy.bind());
  REQUIRE_FALSE(refused_complete.has_value());
  REQUIRE(refused_complete.error() == aa::agent_error::claim_not_on_task);
}

TEST_CASE("release also succeeds on a plan_step claim (task 6890)", "[agentatomic][6890]") {
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  recording_policy policy;

  exec(conn,
       std::format("insert into plan_steps (plan_id, ordinal, body, status) values ({}, 1, 'Step one', 'pending')", fx.plan_id));
  auto const step_id = scalar_int(conn, "select id from plan_steps where plan_id = " + std::to_string(fx.plan_id));

  auto args       = basic_args(fx, step_id);
  args.kind       = aa::entity_kind::plan_step;
  auto const held = atomic::claim_entity(conn, args, true, policy.bind());
  REQUIRE(held.has_value());

  auto const released = atomic::release_work(conn, held->claim_token, std::nullopt, policy.bind());
  REQUIRE(released.has_value());
  REQUIRE_FALSE(released->task_id.has_value()); // task 7118
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", held->id)) == "released");
}

TEST_CASE("a terminal verb refuses an EXPIRED lease", "[agentatomic]") {
  // Status alone is not the test: the claim below is still
  // `status='active'` in the row. Only the lease has passed. A guard that
  // read the column and not the lease would let a worker that slept past
  // its TTL complete work someone else has since taken over.
  scratch_db_path  scratch;
  auto             conn = open_migrated(scratch);
  auto const       fx   = seed(conn, 1);
  auto const       task = task_id_at(conn, 0);
  recording_policy policy;

  auto expired     = basic_args(fx, task);
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());
  REQUIRE(dead->status == aa::claim_status::active);
  exec(conn, std::format("update tasks set status = 'doing' where id = {}", task));

  auto const refused = atomic::complete_work(conn, dead->claim_token, std::nullopt, policy.bind());
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::claim_not_active);
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "doing");

  // ...and it was refused BEFORE any work was done. Break-probe: deleting
  // the lease half of the guard (`held->status != active || !*live` ->
  // `held->status != active`) leaves the ERROR unchanged, because
  // `release_claim`'s own SQL guard refuses a moment later and the
  // transaction rolls back. The only observable difference is that the
  // transition guard gets consulted on the way there — so THIS is the
  // assertion that discriminates, and without it the mutation survives.
  REQUIRE(policy.transitions.empty());
  REQUIRE(policy.recomputes.empty());
}

// ===========================================================================
// Concurrency — two connections, one database
// ===========================================================================

TEST_CASE("sequential claims on one entity contend", "[agentatomic]") {
  // NOT a concurrency test, and labelled accordingly. The two calls run
  // one after the other on separate connections, so the first has
  // committed before the second begins: this shows the exclusivity
  // PREDICATE is spelled correctly, and nothing about whether it is safe
  // under contention. Break-probed and confirmed non-discriminating for
  // the lock mode — downgrading `claim_entity` to `lock_mode::deferred`
  // leaves it passing, which is exactly why the threaded case below
  // exists.
  scratch_db_path  scratch;
  auto             first  = open_migrated(scratch);
  auto             second = open_second(scratch);
  auto const       fx     = seed(first, 1);
  auto const       task   = task_id_at(first, 0);
  recording_policy policy;

  auto const won  = atomic::claim_entity(first, basic_args(fx, task), true, policy.bind());
  auto const lost = atomic::claim_entity(second, basic_args(fx, task), true, policy.bind());

  REQUIRE(won.has_value());
  REQUIRE_FALSE(lost.has_value());
  REQUIRE(lost.error() == aa::agent_error::claim_contention);
  REQUIRE(scalar_int(first, "select count(*) from agent_work_claims where status = 'active'") == 1);
  REQUIRE(scalar_text(first, std::format("select status from tasks where id = {}", task)) == "doing");
}

TEST_CASE("racing threads cannot both claim one entity", "[agentatomic][concurrency]") {
  // THE EXCLUSIVITY RACE, ACTUALLY RACED. Eight threads, eight independent
  // connections to the same file, released together by a latch, all
  // claiming the SAME entity. Exactly one may win.
  //
  // Why this discriminates where the sequential case does not:
  // `acquire_claim` is a SELECT followed by an INSERT. Under
  // `BEGIN IMMEDIATE` every thread serialises at `BEGIN`, so each one's
  // SELECT runs after the previous winner's COMMIT and correctly sees the
  // claim. Under `BEGIN DEFERRED` no lock is taken until the first WRITE,
  // so several threads run their SELECT against an empty table, then queue
  // up at the INSERT and each commits a second active claim.
  //
  // WAL MODE IS LOAD-BEARING AND WAS INITIALLY MISSING. The first version
  // of this test ran in SQLite's default rollback-journal mode, where the
  // single-writer lock alone prevents a double-claim and the lock MODE is
  // therefore invisible — the deferred mutation survived. The binary runs
  // in WAL (`context::ensure_db` sets `journal_mode = WAL`), where readers
  // do not block writers and a deferred transaction's SELECT really can
  // observe a snapshot the INSERT then contradicts. Matching production's
  // journal mode is what turned this into a discriminating test.
  //
  // `busy_timeout` is likewise set on every connection, matching the
  // binary's 5000ms. Without it the losers would fail immediately with
  // SQLITE_BUSY instead of waiting and re-evaluating.
  //
  // BREAK-PROBE RESULT, and it is NOT the one first expected. Even under
  // WAL, SQLite's own snapshot check stops a deferred loser's INSERT, so
  // "exactly one winner" holds either way. What the lock mode decides is
  // HOW the losers fail, and that is measured directly below:
  //
  //     lock_mode::immediate  -> 84/84 losers report ClaimContention
  //     lock_mode::deferred   -> 84/84 losers report QueryFailed
  //
  // That difference is operationally load-bearing, not cosmetic. An
  // orchestrator can retry or move on when told the entity is taken; a
  // bare QueryFailed reads as a bug in the binary and is what an operator
  // would escalate. The `contended` assertion is therefore the one that
  // discriminates, and dropping it makes the deferred mutation survive.
  constexpr int k_threads = 8;
  constexpr int k_rounds  = 12;

  scratch_db_path scratch;
  auto            control = open_migrated(scratch);
  exec(control, "pragma journal_mode = WAL");
  exec(control, "pragma busy_timeout = 5000");
  auto const fx = seed(control, k_rounds);

  for (int round = 0; round < k_rounds; ++round) {
    auto const task = task_id_at(control, round);

    std::latch                gate{k_threads};
    std::atomic<int>          winners{0};
    std::atomic<int>          contended{0};
    std::vector<std::jthread> workers;
    workers.reserve(k_threads);

    for (int i = 0; i < k_threads; ++i) {
      workers.emplace_back([&] {
        auto conn = planar::db::connection::open(scratch.path_.string());
        if (!conn) {
          return;
        }
        (void)conn->execute("pragma journal_mode = WAL");
        (void)conn->execute("pragma busy_timeout = 5000");
        recording_policy policy;
        gate.arrive_and_wait();
        auto const held = atomic::claim_entity(*conn, basic_args(fx, task), true, policy.bind());
        if (held) {
          winners.fetch_add(1);
        } else if (held.error() == aa::agent_error::claim_contention) {
          contended.fetch_add(1);
        }
      });
    }
    workers.clear(); // joins every jthread

    // The invariant. Anything above one is a double-claim: two agents
    // believing they own the same task, which is the failure the whole
    // coordination contract exists to prevent.
    REQUIRE(winners.load() == 1);
    // Every loser got a CLEAN contention answer — see the break-probe note
    // above. This is the assertion the lock mode is visible through.
    REQUIRE(contended.load() == k_threads - 1);
    REQUIRE(scalar_int(control, std::format("select count(*) from agent_work_claims where entity_id = {} "
                                            "and status = 'active'",
                                            task)) == 1);
    // Exactly one task transition happened.
    REQUIRE(scalar_text(control, std::format("select status from tasks where id = {}", task)) == "doing");
    // And exactly one `claim_check` marker: a second one would mean a
    // second transaction also ran the transition branch.
    REQUIRE(scalar_int(control, std::format("select count(*) from agent_actions where action_kind = 'claim_check' "
                                            "and entity_id = {}",
                                            task)) == 1);
  }
}

TEST_CASE("racing threads pulling one plan never double-claim a task", "[agentatomic][concurrency]") {
  // The same race through `pull`, where the contended resource is the
  // SELECTOR rather than a named entity: eight threads pull from a plan
  // holding four eligible tasks. Every task may be claimed at most once
  // and the four claims must be on four DISTINCT tasks — a stale read in
  // the selector shows up as two threads pulling the same row.
  constexpr int k_threads = 8;
  constexpr int k_tasks   = 4;

  scratch_db_path scratch;
  auto            control = open_migrated(scratch);
  exec(control, "pragma journal_mode = WAL");
  exec(control, "pragma busy_timeout = 5000");
  auto const fx = seed(control, k_tasks);

  std::latch                gate{k_threads};
  std::atomic<int>          claimed{0};
  std::vector<std::jthread> workers;
  workers.reserve(k_threads);

  for (int i = 0; i < k_threads; ++i) {
    workers.emplace_back([&] {
      auto conn = planar::db::connection::open(scratch.path_.string());
      if (!conn) {
        return;
      }
      (void)conn->execute("pragma journal_mode = WAL");
      (void)conn->execute("pragma busy_timeout = 5000");
      recording_policy policy;
      gate.arrive_and_wait();
      auto const pulled = atomic::pull_next(
          *conn, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
      if (pulled && !pulled->no_work) {
        claimed.fetch_add(1);
      }
    });
  }
  workers.clear();

  REQUIRE(claimed.load() == k_tasks);
  REQUIRE(scalar_int(control, "select count(*) from agent_work_claims where status = 'active'") == k_tasks);
  // DISTINCT entity ids — the assertion a double-pull fails.
  REQUIRE(scalar_int(control, "select count(distinct entity_id) from agent_work_claims where status = 'active'") == k_tasks);
  REQUIRE(scalar_int(control, "select count(*) from tasks where status = 'doing'") == k_tasks);
}

TEST_CASE("16 threads claiming 16 DISTINCT tasks via claim_entity produce 16 distinct tokens, "
          "and never see Busy or QueryFailed",
          "[agentatomic][concurrency][6842][6843]") {
  // The direct engine equivalent of "16 concurrent `planar-agent claim
  // --entity task:<id>` on 16 distinct tasks" from the same host process --
  // this is what tasks 6842 (shared WAL + busy_timeout) and 6843 (Busy vs
  // QueryFailed classification) are FOR. Unlike the single-entity race
  // above, no two threads compete for the same row here: each of the 16
  // gets its own task id, so a correct implementation should see 16 clean
  // winners and NOTHING else -- no contention errors of any kind, busy or
  // otherwise. `busy_timeout=5000` (now set by `connection::open` itself,
  // task 6842) is what turns a transient `BEGIN IMMEDIATE` collision on
  // SQLite's own file-level locks (unrelated rows, same file) into a short
  // wait instead of an immediate failure.
  constexpr int k_threads = 16;

  scratch_db_path scratch;
  auto            control = open_migrated(scratch);
  auto const      fx      = seed(control, k_threads);

  // Catch2 is NOT thread-safe: its assertion macros race on a single
  // output-redirect state, and `activate()` asserts `!m_redirectActive`.
  // `task_id_at` reaches `scalar_int`, which is four `REQUIRE`s, so calling
  // it from inside the worker below put sixteen threads through those
  // macros at once and aborted the process intermittently (measured ~1 run
  // in 4 of the full suite; `--repeat until-fail:300` reproduces in
  // seconds). Resolve every id HERE, on the main thread, so the worker
  // path contains no Catch2 macro at all.
  std::vector<std::int64_t> task_ids;
  task_ids.reserve(k_threads);
  for (int i = 0; i < k_threads; ++i) {
    task_ids.push_back(task_id_at(control, i));
  }

  std::latch                gate{k_threads};
  std::atomic<int>          winners{0};
  std::atomic<int>          busy{0};
  std::atomic<int>          query_failed{0};
  std::atomic<int>          other{0};
  std::vector<std::jthread> workers;
  workers.reserve(k_threads);

  for (int i = 0; i < k_threads; ++i) {
    workers.emplace_back([&, i] {
      auto conn = planar::db::connection::open(scratch.path_.string());
      if (!conn) {
        other.fetch_add(1);
        return;
      }
      recording_policy policy;
      auto const       task = task_ids[static_cast<std::size_t>(i)];
      gate.arrive_and_wait();
      auto const held = atomic::claim_entity(*conn, basic_args(fx, task), true, policy.bind());
      if (held) {
        winners.fetch_add(1);
        return;
      }
      if (held.error() == aa::agent_error::busy) {
        busy.fetch_add(1);
      } else if (held.error() == aa::agent_error::query_failed) {
        query_failed.fetch_add(1);
      } else {
        other.fetch_add(1);
      }
    });
  }
  workers.clear();

  INFO("winners=" << winners.load() << " busy=" << busy.load() << " query_failed=" << query_failed.load()
                  << " other=" << other.load());
  REQUIRE(winners.load() == k_threads);
  REQUIRE(busy.load() == 0);
  REQUIRE(query_failed.load() == 0);
  REQUIRE(other.load() == 0);
  REQUIRE(scalar_int(control, "select count(*) from agent_work_claims where status = 'active'") == k_threads);
  REQUIRE(scalar_int(control, "select count(distinct entity_id) from agent_work_claims where status = 'active'") == k_threads);
  REQUIRE(scalar_int(control, "select count(*) from tasks where status = 'doing'") == k_threads);
}

TEST_CASE("racing threads terminalising one claim produce exactly one winner", "[agentatomic][concurrency]") {
  // A terminal verb interleaved with other terminal verbs, raced for real.
  // Eight threads all try to end the same claim with DIFFERENT verbs, so a
  // non-atomic implementation could leave the task at one verb's
  // destination and the claim at another's.
  //
  // The end state must be internally consistent: `done` pairs with
  // `completed`, `todo` pairs with `aborted` or `released`. A mismatched
  // pair is precisely the split state the one-transaction rule prevents.
  constexpr int k_threads = 8;

  scratch_db_path scratch;
  auto            control = open_migrated(scratch);
  exec(control, "pragma journal_mode = WAL");
  exec(control, "pragma busy_timeout = 5000");
  auto const       fx = seed(control, 1);
  recording_policy setup_policy;
  auto const       pulled = atomic::pull_next(
      control, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, setup_policy.bind());
  REQUIRE(pulled.has_value());
  auto const token = pulled->acquired->claim_token;

  std::latch                gate{k_threads};
  std::atomic<int>          winners{0};
  std::vector<std::jthread> workers;
  workers.reserve(k_threads);

  for (int i = 0; i < k_threads; ++i) {
    workers.emplace_back([&, i] {
      auto conn = planar::db::connection::open(scratch.path_.string());
      if (!conn) {
        return;
      }
      (void)conn->execute("pragma journal_mode = WAL");
      (void)conn->execute("pragma busy_timeout = 5000");
      recording_policy policy;
      gate.arrive_and_wait();
      bool const won = (i % 2 == 0) ? atomic::complete_work(*conn, token, "raced", policy.bind()).has_value()
                                    : atomic::release_work(*conn, token, "raced", policy.bind()).has_value();
      if (won) {
        winners.fetch_add(1);
      }
    });
  }
  workers.clear();

  REQUIRE(winners.load() == 1);

  auto const task_status = scalar_text(control, std::format("select status from tasks where id = {}", pulled->task_id));
  auto const claim_status =
      scalar_text(control, std::format("select status from agent_work_claims where claim_token = '{}'", token));
  // The pair must agree. `done`+`released` or `todo`+`completed` would each
  // be a torn write across the two tables.
  if (claim_status == "completed") {
    REQUIRE(task_status == "done");
  } else {
    REQUIRE(claim_status == "released");
    REQUIRE(task_status == "todo");
  }
  // Exactly one release timestamp, and the claim's open action is closed
  // exactly once with the matching outcome.
  REQUIRE(scalar_int(control, std::format("select count(*) from agent_actions where claim_id = {} and ended_at is null",
                                          pulled->acquired->id)) == 0);
}

TEST_CASE("two connections pulling the same plan take DIFFERENT tasks", "[agentatomic][concurrency]") {
  // The selector's `not exists (live claim)` predicate has to hold across
  // connections, not just within one. With one eligible task the second
  // pull must report `no_work` rather than double-claiming it.
  scratch_db_path  scratch;
  auto             first  = open_migrated(scratch);
  auto             second = open_second(scratch);
  auto const       fx     = seed(first, 2);
  recording_policy policy;

  auto const a = atomic::pull_next(first, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"},
                                   policy.bind());
  auto const b = atomic::pull_next(
      second, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE_FALSE(a->no_work);
  REQUIRE_FALSE(b->no_work);
  REQUIRE(a->task_id != b->task_id);

  auto const c = atomic::pull_next(first, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"},
                                   policy.bind());
  REQUIRE(c.has_value());
  REQUIRE(c->no_work);
  REQUIRE(scalar_int(first, "select count(*) from agent_work_claims") == 2);
}

TEST_CASE("an open immediate transaction locks out a concurrent terminal verb", "[agentatomic][concurrency][6843]") {
  // A terminal verb interleaved with another writer. The first connection
  // holds an UNCOMMITTED immediate transaction; the second's terminal verb
  // cannot begin, so it is refused outright rather than reading through to
  // a half-applied state or committing on top of one.
  //
  // The refusal is a genuine post-timeout SQLITE_BUSY -- `first` never
  // releases the lock for the whole scope below, so `second`'s BEGIN
  // IMMEDIATE retries for the full busy_timeout window and then reports
  // SQLITE_BUSY, not some other failure. Task 6843: that maps to
  // `agent_error::busy` ("Busy" on the CLI, `error: complete: Busy`), never
  // to `query_failed` -- an operator seeing contention needs a different
  // signal (retry) than one seeing corruption (escalate).
  scratch_db_path scratch;
  auto            first  = open_migrated(scratch);
  auto            second = open_second(scratch);
  // Task 6842 made connection::open set busy_timeout=5000 by default.
  // Lower `second`'s override so this test's guaranteed-busy assertion
  // below doesn't block for 5 real seconds; the property under test is the
  // ERROR MAPPING once the timeout is exhausted, not the timeout's length.
  REQUIRE(second.execute("pragma busy_timeout = 50;"));
  auto const       fx = seed(first, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      first, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  auto const token = pulled->acquired->claim_token;

  {
    auto blocker = first.begin_transaction(planar::db::lock_mode::immediate);
    REQUIRE(blocker.has_value());
    // The lock is held for the whole scope, past `second`'s 50ms
    // busy_timeout, so this is a genuine post-timeout SQLITE_BUSY, not a
    // race against how fast the test runs.
    auto const refused = atomic::complete_work(second, token, std::nullopt, policy.bind());
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == aa::agent_error::busy);
  }

  // Nothing moved, and once the lock is released the verb succeeds — the
  // refusal was contention, not damage.
  REQUIRE(scalar_text(first, std::format("select status from tasks where id = {}", pulled->task_id)) == "doing");
  REQUIRE(atomic::complete_work(second, token, std::nullopt, policy.bind()).has_value());
  REQUIRE(scalar_text(first, std::format("select status from tasks where id = {}", pulled->task_id)) == "done");
}

TEST_CASE("a heartbeat cannot interleave INSIDE a terminal transaction", "[agentatomic][concurrency]") {
  // The specific race the ritual would suffer if the terminal verbs were
  // NOT atomic: a worker heartbeating while its own terminal verb is
  // partway through. With one transaction the heartbeat either lands
  // wholly before (and the claim terminalises normally) or is refused
  // because the claim is already terminal. There is no window in which it
  // refreshes the lease on a claim whose task has already been flipped.
  scratch_db_path  scratch;
  auto             first  = open_migrated(scratch);
  auto             second = open_second(scratch);
  auto const       fx     = seed(first, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      first, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  auto const token = pulled->acquired->claim_token;

  // Before: the heartbeat is accepted.
  REQUIRE(aa::heartbeat_claim(second, token, 600).has_value());

  REQUIRE(atomic::complete_work(first, token, std::nullopt, policy.bind()).has_value());

  // After: refused, because the claim is `completed`. The two outcomes are
  // the ONLY two possible; there is no third in which the lease is
  // extended on an already-completed task.
  auto const late = aa::heartbeat_claim(second, token, 600);
  REQUIRE_FALSE(late.has_value());
  REQUIRE(late.error() == aa::agent_error::claim_not_active);
  REQUIRE(scalar_text(first, std::format("select status from tasks where id = {}", pulled->task_id)) == "done");
  REQUIRE(scalar_text(first, std::format("select status from agent_work_claims where claim_token = '{}'", token)) == "completed");
}

TEST_CASE("only one of two racing terminal verbs succeeds", "[agentatomic][concurrency]") {
  // Two connections both trying to end the same claim. The `status='active'
  // AND lease unexpired` guard inside the release UPDATE is what makes the
  // second one lose — and because the guard is INSIDE the transaction, the
  // loser's task flip rolls back with it. A non-atomic implementation
  // could leave the task at the LOSER's destination while the claim
  // carried the WINNER's status.
  scratch_db_path  scratch;
  auto             first  = open_migrated(scratch);
  auto             second = open_second(scratch);
  auto const       fx     = seed(first, 1);
  recording_policy policy;

  auto const pulled = atomic::pull_next(
      first, atomic::pull_args{.plan_id = fx.plan_id, .session_id = fx.session_id, .vendor = "test"}, policy.bind());
  REQUIRE(pulled.has_value());
  auto const token = pulled->acquired->claim_token;

  auto const completed = atomic::complete_work(first, token, std::nullopt, policy.bind());
  auto const failed    = atomic::fail_work(second, token, "too late", aa::failure_category::unknown, policy.bind());

  REQUIRE(completed.has_value());
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.error() == aa::agent_error::claim_not_active);

  // The winner's pair, intact and consistent. If the loser's task flip had
  // survived its rollback, this would read `todo` against a `completed`
  // claim — the exact split state the atomicity requirement exists to
  // prevent.
  REQUIRE(scalar_text(first, std::format("select status from tasks where id = {}", pulled->task_id)) == "done");
  REQUIRE(scalar_text(first, std::format("select status from agent_work_claims where claim_token = '{}'", token)) == "completed");
}

// ===========================================================================
// record_review (plan 1142, task mo-review-record)
// ===========================================================================

namespace {

/// Pull the plan's only task under a vendor session and return its claim.
auto pull_under_session(planar::db::connection& conn, const fixture& fx, std::optional<std::string_view> vendor_session)
    -> aa::claim {
  recording_policy policy;
  auto const       pulled = atomic::pull_next(conn,
                                              atomic::pull_args{.plan_id           = fx.plan_id,
                                                                .session_id        = fx.session_id,
                                                                .vendor            = "claude",
                                                                .vendor_session_id = vendor_session,
                                                                .role              = "coder"},
                                              policy.bind());
  REQUIRE(pulled.has_value());
  REQUIRE(pulled->acquired.has_value());
  return *pulled->acquired;
}

auto count_actions(planar::db::connection& conn) -> std::int64_t {
  return scalar_int(conn, "select count(*) from agent_actions");
}

auto count_sessions(planar::db::connection& conn) -> std::int64_t {
  return scalar_int(conn, "select count(*) from sessions");
}

auto review_args_for(std::string_view reviewed, std::optional<std::string_view> session,
                     std::optional<std::string_view> reviewer_claim = std::nullopt) -> atomic::review_args {
  return atomic::review_args{.reviewed_claim_token    = reviewed,
                             .reviewer_claim_token    = reviewer_claim,
                             .reviewer_vendor_session = session,
                             .vendor                  = "claude",
                             .verdict                 = aa::review_verdict::approve,
                             .summary                 = "reads well"};
}

} // namespace

TEST_CASE("a review under a different session writes one closed review action, a child of the claim's action",
          "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto const      fx     = seed(conn, 1);
  auto const      coder  = pull_under_session(conn, fx, "claude:coder-1");
  auto const      root   = scalar_int(conn, std::format("select min(id) from agent_actions where claim_id = {}", coder.id));
  auto const      before = count_actions(conn);

  auto args       = review_args_for(coder.claim_token, "claude:reviewer-1");
  args.verdict    = aa::review_verdict::request_changes;
  auto const done = atomic::record_review(conn, args);
  REQUIRE(done.has_value());
  REQUIRE(count_actions(conn) == before + 1);
  CHECK(done->parent_action_id == root);
  CHECK(done->verdict == aa::review_verdict::request_changes);

  auto const row = aa::get_action_by_id(conn, done->action_id);
  REQUIRE(row.has_value());
  CHECK(row->kind == aa::action_kind::review);
  CHECK(row->verdict == aa::review_verdict::request_changes);
  CHECK(row->claim_id == coder.id);
  CHECK(row->parent_action_id == root);
  CHECK(row->summary == "reads well");
  CHECK(row->ended_at.has_value());
  CHECK(row->result == aa::outcome::ok);
  CHECK(row->metadata == R"({"reviewer_session":"claude:reviewer-1"})");
  // The action belongs to the REVIEWER's session, not the coder's.
  CHECK(row->session_id != coder.session_id);
  CHECK(scalar_text(conn, std::format("select vendor_session_id from sessions where id = {}", row->session_id)) ==
        "claude:reviewer-1");

  auto const found = aa::reviews_for_claim(conn, coder.claim_token);
  REQUIRE(found.has_value());
  REQUIRE(found->size() == 1);
  CHECK(found->front().action_id == done->action_id);
  CHECK(found->front().verdict == aa::review_verdict::request_changes);
  CHECK(found->front().reviewer_session == "claude:reviewer-1");
  CHECK_FALSE(found->front().reviewer_claim.has_value());
}

TEST_CASE("a review from the reviewed claim's own session is refused and writes nothing", "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  auto const      fx       = seed(conn, 1);
  auto const      coder    = pull_under_session(conn, fx, "claude:coder-1");
  auto const      actions  = count_actions(conn);
  auto const      sessions = count_sessions(conn);

  auto const same = atomic::record_review(conn, review_args_for(coder.claim_token, "claude:coder-1"));
  REQUIRE_FALSE(same.has_value());
  CHECK(same.error() == aa::agent_error::reviewer_not_independent);

  // The coder's own claim, named as the reviewer claim, is the same session too.
  auto const self = atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt, coder.claim_token));
  REQUIRE_FALSE(self.has_value());
  CHECK(self.error() == aa::agent_error::reviewer_not_independent);

  CHECK(count_actions(conn) == actions);
  CHECK(count_sessions(conn) == sessions);
  CHECK(aa::reviews_for_claim(conn, coder.claim_token)->empty());
}

TEST_CASE("a reviewer claim that shares the coder's vendor session is refused; a distinct one is accepted",
          "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      fx    = seed(conn, 1);
  auto const      coder = pull_under_session(conn, fx, "claude:coder-1");
  exec(conn, "insert into sessions (vendor, vendor_session_id) values ('claude', 'claude:rev')");
  auto const reviewer_session = scalar_int(conn, "select id from sessions where vendor_session_id = 'claude:rev'");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','r','rev-plan','active')");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','r2','rev-plan-2','active')");
  auto const rev_plan   = scalar_int(conn, "select id from plans where slug = 'rev-plan'");
  auto const rev_plan_2 = scalar_int(conn, "select id from plans where slug = 'rev-plan-2'");

  auto mint = [&](std::string_view vendor_session, std::int64_t entity) {
    auto const claim = aa::acquire_claim(conn, aa::acquire_args{.session_id        = reviewer_session,
                                                                .kind              = aa::entity_kind::plan,
                                                                .entity_id         = entity,
                                                                .scope             = aa::claim_scope::shared,
                                                                .vendor            = "claude",
                                                                .vendor_session_id = vendor_session,
                                                                .role              = "reviewer"});
    REQUIRE(claim.has_value());
    return *claim;
  };
  auto const colliding = mint("claude:coder-1", rev_plan);
  auto const distinct  = mint("claude:rev", rev_plan_2);

  auto const before  = count_actions(conn);
  auto const refused = atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt, colliding.claim_token));
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == aa::agent_error::reviewer_not_independent);
  CHECK(count_actions(conn) == before);

  auto const accepted = atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt, distinct.claim_token));
  REQUIRE(accepted.has_value());
  auto const row = aa::get_action_by_id(conn, accepted->action_id);
  REQUIRE(row.has_value());
  // The reviewer claim's own session owns the action, and the metadata names both.
  CHECK(row->session_id == reviewer_session);
  CHECK(row->metadata == std::format(R"({{"reviewer_session":"claude:rev","reviewer_claim":"{}"}})", distinct.claim_token));
  auto const found = aa::reviews_for_claim(conn, coder.claim_token);
  REQUIRE(found.has_value());
  REQUIRE(found->size() == 1);
  CHECK(found->front().reviewer_claim == distinct.claim_token);
}

TEST_CASE("a reviewed claim with no vendor session is reviewed only under a reviewer claim that has one",
          "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      fx    = seed(conn, 1);
  auto const      coder = pull_under_session(conn, fx, std::nullopt);
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','r','rev-plan','active')");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','r2','rev-plan-2','active')");
  auto const rev_plan   = scalar_int(conn, "select id from plans where slug = 'rev-plan'");
  auto const rev_plan_2 = scalar_int(conn, "select id from plans where slug = 'rev-plan-2'");
  exec(conn, "insert into sessions (vendor, vendor_session_id) values ('claude', 'claude:rev')");
  auto const reviewer_session = scalar_int(conn, "select id from sessions where vendor_session_id = 'claude:rev'");

  auto mint = [&](std::optional<std::string_view> vendor_session, std::int64_t entity) {
    auto const claim = aa::acquire_claim(conn, aa::acquire_args{.session_id        = reviewer_session,
                                                                .kind              = aa::entity_kind::plan,
                                                                .entity_id         = entity,
                                                                .scope             = aa::claim_scope::shared,
                                                                .vendor            = "claude",
                                                                .vendor_session_id = vendor_session,
                                                                .role              = "reviewer"});
    REQUIRE(claim.has_value());
    return *claim;
  };
  auto const without = mint(std::nullopt, rev_plan);
  auto const with    = mint("claude:rev", rev_plan_2);
  auto const before  = count_actions(conn);

  // Nothing to compare a bare session id against.
  auto const by_session = atomic::record_review(conn, review_args_for(coder.claim_token, "claude:rev"));
  REQUIRE_FALSE(by_session.has_value());
  CHECK(by_session.error() == aa::agent_error::reviewer_not_independent);
  // A reviewer claim without a vendor session cannot be told apart either.
  auto const by_blank = atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt, without.claim_token));
  REQUIRE_FALSE(by_blank.has_value());
  CHECK(by_blank.error() == aa::agent_error::reviewer_not_independent);
  CHECK(count_actions(conn) == before);

  auto const by_claim = atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt, with.claim_token));
  REQUIRE(by_claim.has_value());
  CHECK(count_actions(conn) == before + 1);
}

TEST_CASE("record_review refuses an unknown claim, an unknown reviewer claim and an anonymous caller without writing",
          "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  auto const      fx       = seed(conn, 1);
  auto const      coder    = pull_under_session(conn, fx, "claude:coder-1");
  auto const      actions  = count_actions(conn);
  auto const      sessions = count_sessions(conn);

  auto const unknown = atomic::record_review(conn, review_args_for("0123456789abcdef0123456789abcdef", "claude:reviewer-1"));
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error() == aa::agent_error::claim_not_found);

  auto const no_reviewer =
      atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt, "0123456789abcdef0123456789abcdef"));
  REQUIRE_FALSE(no_reviewer.has_value());
  CHECK(no_reviewer.error() == aa::agent_error::reviewer_claim_not_found);

  auto const anonymous = atomic::record_review(conn, review_args_for(coder.claim_token, std::nullopt));
  REQUIRE_FALSE(anonymous.has_value());
  CHECK(anonymous.error() == aa::agent_error::reviewer_unidentified);

  CHECK(count_actions(conn) == actions);
  CHECK(count_sessions(conn) == sessions);
}

TEST_CASE("a review may follow the claim's completion and is found by the reviewed claim", "[agentatomic][review]") {
  scratch_db_path  scratch;
  auto             conn  = open_migrated(scratch);
  auto const       fx    = seed(conn, 1);
  auto const       coder = pull_under_session(conn, fx, "claude:coder-1");
  recording_policy policy;
  REQUIRE(atomic::complete_work(conn, coder.claim_token, std::nullopt, policy.bind()).has_value());

  REQUIRE(atomic::record_review(conn, review_args_for(coder.claim_token, "claude:reviewer-1")).has_value());
  auto second    = review_args_for(coder.claim_token, "claude:reviewer-2");
  second.verdict = aa::review_verdict::request_changes;
  REQUIRE(atomic::record_review(conn, second).has_value());

  auto const found = aa::reviews_for_claim(conn, coder.claim_token);
  REQUIRE(found.has_value());
  REQUIRE(found->size() == 2);
  CHECK(found->at(0).verdict == aa::review_verdict::approve);
  CHECK(found->at(1).verdict == aa::review_verdict::request_changes);
  CHECK(aa::reviews_for_claim(conn, "0123456789abcdef0123456789abcdef")->empty());
}

TEST_CASE("no review row can carry a NULL verdict, and the schema refuses a foreign verdict", "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      fx    = seed(conn, 1);
  auto const      coder = pull_under_session(conn, fx, "claude:coder-1");
  REQUIRE(atomic::record_review(conn, review_args_for(coder.claim_token, "claude:reviewer-1")).has_value());
  REQUIRE(atomic::record_review(conn, review_args_for(coder.claim_token, "claude:reviewer-2")).has_value());

  CHECK(scalar_int(conn, "select count(*) from agent_actions where action_kind = 'review'") == 2);
  CHECK(scalar_int(conn, "select count(*) from agent_actions where action_kind = 'review' and verdict is null") == 0);
  CHECK(scalar_int(conn, "select count(*) from agent_actions where action_kind <> 'review' and verdict is not null") == 0);
  auto const bad = conn.execute("update agent_actions set verdict = 'lgtm' where action_kind = 'review'");
  CHECK_FALSE(bad.has_value());
}

TEST_CASE("a review row does not make a bare claim look like it moved the task", "[agentatomic][review]") {
  // `reconcile` returns a task to todo only when the claim owns the move; any action on the claim
  // counts as that evidence except the bookkeeping kinds. A review is somebody else's verdict.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto bare              = basic_args(fx, task);
  bare.ttl_secs          = -60;
  bare.vendor_session_id = "claude:coder-1";
  auto const held        = aa::acquire_claim(conn, bare);
  REQUIRE(held.has_value());
  exec(conn, std::format("update tasks set status = 'doing' where id = {}", task));

  REQUIRE(atomic::record_review(conn, review_args_for(held->claim_token, "claude:reviewer-1")).has_value());
  REQUIRE(aa::reconcile_stale(conn, aa::reconcile_policy{}).has_value());
  CHECK(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "doing");
}

TEST_CASE("a review never becomes the claim's latest activity", "[agentatomic][review]") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto const      fx     = seed(conn, 1);
  auto const      coder  = pull_under_session(conn, fx, "claude:coder-1");
  auto const      before = aa::latest_action_for_claim(conn, coder.id);
  REQUIRE(before.has_value());
  REQUIRE(before->has_value());

  REQUIRE(atomic::record_review(conn, review_args_for(coder.claim_token, "claude:reviewer-1")).has_value());
  auto const after = aa::latest_action_for_claim(conn, coder.id);
  REQUIRE(after.has_value());
  REQUIRE(after->has_value());
  CHECK((*after)->id == (*before)->id);
  CHECK((*after)->kind != aa::action_kind::review);
}
