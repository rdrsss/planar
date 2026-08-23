// @file agentactivity.t.cpp
// @brief Unit tests for `planar.engine.runtime.agentactivity` (plan 996,
// task 6038).
//
// Every expectation here was derived by RUNNING
// `zig/zig-out/bin/planar-agent` against a scratch database and reading
// the rows back with sqlite3 — not from the Zig source, and never from
// `--help`. The captures that shaped the sharper assertions:
//
//   * a claim pulled at 16:34:47.223 carried `lease_expires_at`
//     16:44:47.223; a BARE heartbeat at 16:34:56.236 rewrote it to
//     16:44:56.236 — ten minutes from the HEARTBEAT, not from the claim.
//     The lease is reset, never extended. (`heartbeat resets the lease`
//     below.)
//   * `claim --entity task:3` on a `todo` task left the task `doing`. The
//     brief this task shipped under asserted the opposite; the oracle
//     settles it. (Pinned in agentatomic.t.cpp, which owns that verb.)
//   * `reconcile --dry-run --json` reported
//     `"claims_marked_stale":0,"actions_closed":0` alongside a NON-EMPTY
//     `candidates` array, and left every row untouched.
//   * `abort` on a claim whose lease had NOT expired still succeeded —
//     the verb has no lease guard at all.
//
// WHAT THESE TESTS DO NOT PROVE: nothing here is concurrent. Every case
// runs one connection, sequentially. The exclusivity and takeover races
// are agentatomic.t.cpp's job, because they are only meaningful with two
// connections contending; a sequential "claim twice, second one fails"
// case proves the predicate is spelled right, not that it is safe.

#include <catch2/catch_test_macros.hpp>

// `getpid()` — the run sweep's liveness probe is the one thing here that
// asks the kernel a question, so the tests need a pid they KNOW is alive.
#include <unistd.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.agentactivity;

namespace {

namespace aa = planar::engine::runtime::agentactivity;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_agentactivity_test_{}_{}.db",
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

/// A session, a plan and N tasks — the minimum a claim needs, since
/// `agent_work_claims.session_id` is a real FK.
struct fixture {
  std::int64_t session_id = 0;
  std::int64_t plan_id    = 0;
};

auto seed(planar::db::connection& conn, int task_count) -> fixture {
  exec(conn, "insert into sessions (vendor) values ('test')");
  auto const session_id = scalar_int(conn, "select id from sessions where vendor = 'test'");
  exec(conn, "insert into plans (scope_kind, title, slug) values ('global','p','test-plan')");
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

auto basic_args(const fixture& fx, std::int64_t task) -> aa::acquire_args {
  return aa::acquire_args{.session_id = fx.session_id, .kind = aa::entity_kind::task, .entity_id = task, .vendor = "test"};
}

} // namespace

// ===========================================================================
// Enumeration round-trips
// ===========================================================================

TEST_CASE("enum tokens round-trip through the exact stored text", "[agentactivity]") {
  // The tokens are CHECK-constraint members, so a typo here would be
  // rejected by SQLite at write time — but only for the values a test
  // happens to write. This pins all of them at once.
  REQUIRE(aa::entity_kind_from_text(aa::to_text(aa::entity_kind::plan_step)) == aa::entity_kind::plan_step);
  REQUIRE(aa::claim_status_from_text(aa::to_text(aa::claim_status::stale)) == aa::claim_status::stale);
  REQUIRE(aa::claim_scope_from_text(aa::to_text(aa::claim_scope::shared)) == aa::claim_scope::shared);
  REQUIRE(aa::failure_category_from_text(aa::to_text(aa::failure_category::tool_failure)) == aa::failure_category::tool_failure);
  REQUIRE(aa::outcome_from_text(aa::to_text(aa::outcome::timeout)) == aa::outcome::timeout);
  REQUIRE(aa::dirty_state_from_text(aa::to_text(aa::dirty_state::dirty)) == aa::dirty_state::dirty);
  REQUIRE(aa::action_entity_kind_from_text(aa::to_text(aa::action_entity_kind::test_scenario)) ==
          aa::action_entity_kind::test_scenario);

  // `resume_` carries a trailing underscore in C++ and MUST NOT in SQL.
  REQUIRE(aa::to_text(aa::action_kind::resume_) == "resume");
  REQUIRE(aa::action_kind_from_text("resume") == aa::action_kind::resume_);
  // `error_` likewise.
  REQUIRE(aa::to_text(aa::outcome::error_) == "error");

  // Unrecognised text yields nullopt, never a default. A store that
  // silently mapped garbage onto a valid member would make a corrupt row
  // look healthy.
  REQUIRE_FALSE(aa::claim_status_from_text("nonsense").has_value());
  REQUIRE_FALSE(aa::action_kind_from_text("").has_value());
}

TEST_CASE("probe_default is false only for heartbeat and tool_call", "[agentactivity]") {
  REQUIRE_FALSE(aa::probe_default(aa::action_kind::heartbeat));
  REQUIRE_FALSE(aa::probe_default(aa::action_kind::tool_call));
  REQUIRE(aa::probe_default(aa::action_kind::coder));
  REQUIRE(aa::probe_default(aa::action_kind::claim_check));
  REQUIRE(aa::probe_default(aa::action_kind::other));
}

TEST_CASE("error_name emits the operator-visible Zig tags", "[agentactivity]") {
  // These strings are OUTPUT BYTES: the oracle prints
  // `error: complete: ClaimNotActive` and a script greps for it.
  REQUIRE(aa::error_name(aa::agent_error::claim_contention) == "ClaimContention");
  REQUIRE(aa::error_name(aa::agent_error::claim_not_found) == "ClaimNotFound");
  REQUIRE(aa::error_name(aa::agent_error::claim_not_active) == "ClaimNotActive");
  REQUIRE(aa::error_name(aa::agent_error::illegal_transition) == "IllegalTransition");
  REQUIRE(aa::error_name(aa::agent_error::task_not_found) == "TaskNotFound");
  REQUIRE(aa::error_name(aa::agent_error::claim_not_on_task) == "ClaimNotOnTask");
  REQUIRE(aa::error_name(aa::agent_error::worktree_not_found) == "WorktreeNotFound");
  REQUIRE(aa::error_name(aa::agent_error::unknown_status) == "UnknownStatus");
  REQUIRE(aa::error_name(aa::agent_error::query_failed) == "QueryFailed");
}

// ===========================================================================
// acquire_claim
// ===========================================================================

TEST_CASE("acquire_claim mints a 32-hex token and an active lease", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto const held = aa::acquire_claim(conn, basic_args(fx, task));
  REQUIRE(held.has_value());

  // Minted in SQL by `lower(hex(randomblob(16)))`, so 32 lowercase hex
  // characters. The format is opaque to callers, but its SHAPE is the
  // thing a host-side RNG would get wrong.
  REQUIRE(held->claim_token.size() == 32);
  REQUIRE(std::ranges::all_of(held->claim_token, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
  REQUIRE(held->status == aa::claim_status::active);
  REQUIRE(held->scope == aa::claim_scope::exclusive);
  REQUIRE(held->kind == aa::entity_kind::task);
  REQUIRE(held->entity_id == task);

  // Timestamps are SQLite's `%Y-%m-%dT%H:%M:%fZ` — fixed width, so string
  // comparison is chronological. That is what every expiry predicate in
  // this module relies on.
  REQUIRE(held->claimed_at.size() == 24);
  REQUIRE(held->claimed_at.back() == 'Z');
  REQUIRE(held->lease_expires_at > held->claimed_at);
}

TEST_CASE("acquire_claim refuses a live claim and yields to --force", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto const first = aa::acquire_claim(conn, basic_args(fx, task));
  REQUIRE(first.has_value());

  auto const second = aa::acquire_claim(conn, basic_args(fx, task));
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error() == aa::agent_error::claim_contention);

  auto forced           = basic_args(fx, task);
  forced.force          = true;
  auto const taken_over = aa::acquire_claim(conn, forced);
  REQUIRE(taken_over.has_value());
  REQUIRE(taken_over->id != first->id);

  // The displaced claim is marked stale with a reason naming the cause —
  // an operator reading `agent_work_claims` afterwards can tell a takeover
  // from an expiry sweep, which report different `release_reason` text.
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", first->id)) == "stale");
  REQUIRE(scalar_text(conn, std::format("select release_reason from agent_work_claims where id = {}", first->id)) ==
          "force takeover");
}

TEST_CASE("acquire_claim ignores claim_scope when testing contention", "[agentactivity]") {
  // The column NAME promises exclusivity semantics the query does not
  // implement: `has_active_claim`'s WHERE clause never mentions
  // `claim_scope`, so a `shared` claim blocks a second `shared` claim.
  // Reproduced from zig under D2. This test exists so that a future
  // "fix" is a deliberate contract change with a failing test to point at.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto shared_args  = basic_args(fx, task);
  shared_args.scope = aa::claim_scope::shared;
  REQUIRE(aa::acquire_claim(conn, shared_args).has_value());

  auto const second = aa::acquire_claim(conn, shared_args);
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error() == aa::agent_error::claim_contention);
}

TEST_CASE("an EXPIRED claim does not block a new one", "[agentactivity]") {
  // The contention predicate is `status='active' AND lease unexpired`. A
  // lease that has passed keeps `status='active'` until reconcile flips
  // it, so testing the status column alone would wrongly refuse here.
  // A NEGATIVE ttl mints an already-expired claim without sleeping, which
  // is the only way to test this deterministically.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto expired     = basic_args(fx, task);
  expired.ttl_secs = -60;
  auto const stale = aa::acquire_claim(conn, expired);
  REQUIRE(stale.has_value());
  REQUIRE(stale->status == aa::claim_status::active);
  REQUIRE(stale->lease_expires_at < stale->claimed_at);

  auto const fresh = aa::acquire_claim(conn, basic_args(fx, task));
  REQUIRE(fresh.has_value());
}

TEST_CASE("acquire_claim records the locality snapshot, and NULLs an empty one", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);

  auto with_loc = basic_args(fx, task_id_at(conn, 0));
  with_loc.loc  = aa::locality{.repo_root = "/tmp/repo", .branch = "main", .head_sha = "abc123", .dirty = aa::dirty_state::clean};
  auto const probed = aa::acquire_claim(conn, with_loc);
  REQUIRE(probed.has_value());
  REQUIRE(probed->repo_root == "/tmp/repo");
  REQUIRE(probed->branch == "main");
  REQUIRE(probed->head_sha_at_claim == "abc123");
  REQUIRE(probed->dirty_at_claim == aa::dirty_state::clean);

  // A fully-empty snapshot writes SQL NULL into `dirty_at_claim` rather
  // than the string "unknown" — the predicate is "is the WHOLE snapshot
  // empty", and it differs from the one `start_action` uses.
  auto const skipped = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 1)));
  REQUIRE(skipped.has_value());
  REQUIRE_FALSE(skipped->dirty_at_claim.has_value());
  REQUIRE_FALSE(skipped->repo_root.has_value());
}

// ===========================================================================
// heartbeat — the reset-not-extend defect, pinned
// ===========================================================================

TEST_CASE("heartbeat RESETS the lease rather than extending it", "[agentactivity]") {
  // THIS IS THE POINT OF THE TEST. A claim taken with a long TTL and then
  // heartbeated with a short one ends up with the SHORT lease, measured
  // from now — the remaining time is DISCARDED, not added to. That is why
  // `planar-agent heartbeat` without `--ttl` has stranded long-lived
  // claims repeatedly (Planar task 6093).
  //
  // Oracle capture: claim at 16:34:47.223 -> expires 16:44:47.223;
  // bare heartbeat at 16:34:56.236 -> expires 16:44:56.236.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);

  auto long_lease     = basic_args(fx, task_id_at(conn, 0));
  long_lease.ttl_secs = 28800; // 8h, the TTL a long dispatch uses.
  auto const held     = aa::acquire_claim(conn, long_lease);
  REQUIRE(held.has_value());

  auto const refreshed = aa::heartbeat_claim(conn, held->claim_token, 600);
  REQUIRE(refreshed.has_value());

  // The new expiry is EARLIER than the old one. An "extend" implementation
  // could not produce this, and a no-op implementation could not either
  // (it would be equal).
  REQUIRE(refreshed->lease_expires_at < held->lease_expires_at);
  // ...and it is measured from the heartbeat, not from the claim.
  REQUIRE(refreshed->lease_expires_at > refreshed->last_heartbeat_at);
  REQUIRE(refreshed->last_heartbeat_at >= held->last_heartbeat_at);
}

TEST_CASE("heartbeat refuses a terminal or expired claim", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);

  auto const held = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 0)));
  REQUIRE(held.has_value());
  REQUIRE(aa::release_claim(conn, held->claim_token, aa::claim_status::released, std::nullopt, std::nullopt).has_value());

  auto const on_terminal = aa::heartbeat_claim(conn, held->claim_token, 600);
  REQUIRE_FALSE(on_terminal.has_value());
  REQUIRE(on_terminal.error() == aa::agent_error::claim_not_active);

  // An expired-but-still-`active` claim is equally refused: heartbeating a
  // dead lease must not resurrect it, or a worker that slept past its TTL
  // could silently reclaim work someone else has taken over.
  auto expired     = basic_args(fx, task_id_at(conn, 1));
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());
  auto const on_expired = aa::heartbeat_claim(conn, dead->claim_token, 600);
  REQUIRE_FALSE(on_expired.has_value());
  REQUIRE(on_expired.error() == aa::agent_error::claim_not_active);

  // An unknown token is a DIFFERENT error, and the two exit the same way
  // but read differently to an operator.
  auto const unknown = aa::heartbeat_claim(conn, "deadbeef", 600);
  REQUIRE_FALSE(unknown.has_value());
  REQUIRE(unknown.error() == aa::agent_error::claim_not_found);
}

// ===========================================================================
// release / abort
// ===========================================================================

TEST_CASE("release_claim is guarded; abort_claim deliberately is not", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);

  // An expired claim cannot be RELEASED...
  auto expired     = basic_args(fx, task_id_at(conn, 0));
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());
  auto const refused = aa::release_claim(conn, dead->claim_token, aa::claim_status::completed, std::nullopt, std::nullopt);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::claim_not_active);

  // ...but it CAN be aborted. That asymmetry is the whole reason `abort`
  // exists: it is the recovery path for a claim whose owner is gone, and a
  // liveness guard would make it useless in exactly that case.
  auto const aborted = aa::abort_claim(conn, dead->claim_token, "stuck", aa::failure_category::usage_limit);
  REQUIRE(aborted.has_value());
  REQUIRE(aborted->status == aa::claim_status::aborted);
  REQUIRE(aborted->release_reason == "stuck");
  REQUIRE(aborted->category == aa::failure_category::usage_limit);

  // Abort an ALREADY-terminal claim: still succeeds, because there is no
  // status guard either.
  auto const again = aa::abort_claim(conn, dead->claim_token, std::nullopt, std::nullopt);
  REQUIRE(again.has_value());
  // ...and passing no category CLEARS the one recorded a moment ago —
  // `failure_category` is written unconditionally.
  REQUIRE_FALSE(again->category.has_value());

  // Only an unknown token fails.
  auto const missing = aa::abort_claim(conn, "nosuchtoken", std::nullopt, std::nullopt);
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error() == aa::agent_error::claim_not_found);
}

TEST_CASE("release_claim refuses to move a claim back to active", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      held = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 0)));
  REQUIRE(held.has_value());

  auto const refused = aa::release_claim(conn, held->claim_token, aa::claim_status::active, std::nullopt, std::nullopt);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == aa::agent_error::query_failed);
}

// ===========================================================================
// actions
// ===========================================================================

TEST_CASE("start_action / end_action / close_open_actions_for_claim", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);
  auto const      held = aa::acquire_claim(conn, basic_args(fx, task));
  REQUIRE(held.has_value());

  auto const first = aa::start_action(conn, aa::start_action_args{.session_id = fx.session_id,
                                                                  .claim_id   = held->id,
                                                                  .kind       = aa::action_kind::coder,
                                                                  .entity     = aa::action_entity_kind::task,
                                                                  .entity_id  = task,
                                                                  .vendor     = "test"});
  REQUIRE(first.has_value());

  // `latest_open_action_for_claim` is what `action start` uses to build
  // the nesting without the caller tracking parent ids.
  REQUIRE(aa::latest_open_action_for_claim(conn, held->id) == *first);

  // A half-specified entity is refused BEFORE SQLite's own CHECK sees it,
  // so the caller gets this module's error rather than a driver code.
  auto const half = aa::start_action(
      conn, aa::start_action_args{.session_id = fx.session_id, .kind = aa::action_kind::other, .entity_id = 1, .vendor = "test"});
  REQUIRE_FALSE(half.has_value());
  REQUIRE(half.error() == aa::agent_error::query_failed);

  REQUIRE(aa::end_action(conn, *first, aa::outcome::ok, "did it").has_value());
  auto const closed = aa::get_action_by_id(conn, *first);
  REQUIRE(closed.has_value());
  REQUIRE(closed->result == aa::outcome::ok);
  REQUIRE(closed->summary == "did it");
  REQUIRE(closed->ended_at.has_value());

  // Closing an already-closed action is a silent NO-OP, not an error —
  // the UPDATE's `ended_at is null` guard simply matches nothing. The Zig
  // doc comment claims otherwise; the SQL is what ships.
  REQUIRE(aa::end_action(conn, *first, aa::outcome::timeout, "second try").has_value());
  auto const unchanged = aa::get_action_by_id(conn, *first);
  REQUIRE(unchanged.has_value());
  REQUIRE(unchanged->result == aa::outcome::ok);
  REQUIRE(unchanged->summary == "did it");

  // And an absent action id is equally a no-op.
  REQUIRE(aa::end_action(conn, 99999, aa::outcome::ok, std::nullopt).has_value());
  REQUIRE(aa::get_action_by_id(conn, 99999).error() == aa::agent_error::claim_not_found);
}

TEST_CASE("closing actions with an unset summary PRESERVES the existing one", "[agentactivity]") {
  // `summary = coalesce(?, summary)`. This is why `fail` and `release`
  // pass null while `complete` passes `--summary`: the worker's own
  // summary survives a failure rather than being blanked by it.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      held = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 0)));
  REQUIRE(held.has_value());

  auto const opened = aa::start_action(
      conn,
      aa::start_action_args{.session_id = fx.session_id, .claim_id = held->id, .kind = aa::action_kind::coder, .vendor = "test"});
  REQUIRE(opened.has_value());
  exec(conn, std::format("update agent_actions set summary = 'worker note' where id = {}", *opened));

  REQUIRE(aa::close_open_actions_for_claim(conn, held->id, aa::outcome::error_, std::nullopt).has_value());
  auto const closed = aa::get_action_by_id(conn, *opened);
  REQUIRE(closed.has_value());
  REQUIRE(closed->summary == "worker note");
  REQUIRE(closed->result == aa::outcome::error_);
}

TEST_CASE("start_action's dirty predicate differs from acquire_claim's", "[agentactivity]") {
  // Two call sites, two different NULL rules, both reproduced from zig:
  //   claim:  NULL iff dirty==unknown AND repo_root/branch/head_sha all unset
  //   action: NULL iff head_sha unset AND dirty==unknown
  // A snapshot with a repo_root but no head_sha therefore stores 'unknown'
  // on the CLAIM and NULL on the ACTION.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);

  aa::locality partial{.repo_root = "/tmp/repo", .dirty = aa::dirty_state::unknown};

  auto args       = basic_args(fx, task_id_at(conn, 0));
  args.loc        = partial;
  auto const held = aa::acquire_claim(conn, args);
  REQUIRE(held.has_value());
  REQUIRE(held->dirty_at_claim == aa::dirty_state::unknown);

  auto const opened = aa::start_action(
      conn,
      aa::start_action_args{
          .session_id = fx.session_id, .claim_id = held->id, .kind = aa::action_kind::coder, .vendor = "test", .loc = partial});
  REQUIRE(opened.has_value());
  auto const row = aa::get_action_by_id(conn, *opened);
  REQUIRE(row.has_value());
  REQUIRE_FALSE(row->dirty.has_value());
}

// ===========================================================================
// associate_claim_run
// ===========================================================================

TEST_CASE("associate_claim_run reports 0 for an unknown or terminal claim", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      held = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 0)));
  REQUIRE(held.has_value());
  exec(conn, std::format("insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) "
                         "values ({}, 'wf', 'run-1', 1, '/tmp')",
                         fx.plan_id));
  auto const run_id = scalar_int(conn, "select id from workflow_runs where run_identifier = 'run-1'");

  auto const stamped = aa::associate_claim_run(conn, held->claim_token, run_id, "code");
  REQUIRE(stamped.has_value());
  REQUIRE(*stamped == 1);
  auto const reread = aa::get_claim_by_token(conn, held->claim_token);
  REQUIRE(reread.has_value());
  REQUIRE(reread->run_id == run_id);
  REQUIRE(reread->stage == "code");

  // Unknown token: NOT an error. The external harness that calls this is
  // stamping metadata, not asserting ownership.
  auto const missing = aa::associate_claim_run(conn, "nosuchtoken", run_id, std::nullopt);
  REQUIRE(missing.has_value());
  REQUIRE(*missing == 0);

  // Terminal claim: likewise 0, because the UPDATE is guarded on `active`.
  REQUIRE(aa::release_claim(conn, held->claim_token, aa::claim_status::completed, std::nullopt, std::nullopt).has_value());
  auto const on_terminal = aa::associate_claim_run(conn, held->claim_token, run_id, std::nullopt);
  REQUIRE(on_terminal.has_value());
  REQUIRE(*on_terminal == 0);
}

// ===========================================================================
// reconcile
// ===========================================================================

TEST_CASE("reconcile --dry-run finds candidates and writes NOTHING", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);

  auto expired     = basic_args(fx, task_id_at(conn, 0));
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());
  auto const live = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 1)));
  REQUIRE(live.has_value());

  auto const preview = aa::reconcile_stale(conn, aa::reconcile_policy{.dry_run = true});
  REQUIRE(preview.has_value());
  REQUIRE(preview->candidates.size() == 1);
  REQUIRE(preview->candidates[0].id == dead->id);
  // Both counters stay ZERO even though a candidate was found — the oracle
  // reports `"claims_marked_stale":0` alongside a non-empty array, and a
  // reader who conflated the two would think the sweep had run.
  REQUIRE(preview->claims_marked_stale == 0);
  REQUIRE(preview->actions_closed == 0);
  // And nothing moved.
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", dead->id)) == "active");
}

TEST_CASE("reconcile marks expired claims stale and leaves live ones alone", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);

  auto expired     = basic_args(fx, task_id_at(conn, 0));
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());
  auto const live = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 1)));
  REQUIRE(live.has_value());

  auto const swept = aa::reconcile_stale(conn, aa::reconcile_policy{.category = aa::failure_category::usage_limit});
  REQUIRE(swept.has_value());
  REQUIRE(swept->claims_marked_stale == 1);

  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", dead->id)) == "stale");
  REQUIRE(scalar_text(conn, std::format("select release_reason from agent_work_claims where id = {}", dead->id)) ==
          "reconcile: heartbeat expired");
  REQUIRE(scalar_text(conn, std::format("select failure_category from agent_work_claims where id = {}", dead->id)) ==
          "usage_limit");
  // The live claim is untouched — the predicate is the LEASE, not the age.
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", live->id)) == "active");
}

TEST_CASE("reconcile's --stale-after grace holds a barely-expired claim back", "[agentactivity]") {
  // The grace is subtracted from `now` in the comparison, so a claim whose
  // lease passed 60s ago survives a 600s grace and is swept by a 0s one.
  // This is what makes `--stale-after` more than decoration.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);

  auto expired     = basic_args(fx, task_id_at(conn, 0));
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());

  auto const graced = aa::reconcile_stale(conn, aa::reconcile_policy{.stale_after_secs = 600, .dry_run = true});
  REQUIRE(graced.has_value());
  REQUIRE(graced->candidates.empty());

  auto const immediate = aa::reconcile_stale(conn, aa::reconcile_policy{.stale_after_secs = 0, .dry_run = true});
  REQUIRE(immediate.has_value());
  REQUIRE(immediate->candidates.size() == 1);
}

TEST_CASE("reconcile returns a task to todo ONLY when the claim owns the move", "[agentactivity]") {
  // The evidence is an action row on the claim. A claim with NO action
  // (the `--no-transition` primitive) never moved the task and so must not
  // reset it — otherwise a bare claim could yank a task out from under
  // whatever actually set it to `doing`.
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      fx      = seed(conn, 2);
  auto const      owned   = task_id_at(conn, 0);
  auto const      unowned = task_id_at(conn, 1);

  auto owning            = basic_args(fx, owned);
  owning.ttl_secs        = -60;
  auto const with_action = aa::acquire_claim(conn, owning);
  REQUIRE(with_action.has_value());
  REQUIRE(aa::start_action(conn, aa::start_action_args{.session_id = fx.session_id,
                                                       .claim_id   = with_action->id,
                                                       .kind       = aa::action_kind::coder,
                                                       .vendor     = "test"})
              .has_value());

  auto bare     = basic_args(fx, unowned);
  bare.ttl_secs = -60;
  REQUIRE(aa::acquire_claim(conn, bare).has_value());

  // Both tasks are `doing` from the outside.
  exec(conn, std::format("update tasks set status = 'doing' where id in ({}, {})", owned, unowned));

  REQUIRE(aa::reconcile_stale(conn, aa::reconcile_policy{}).has_value());
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", owned)) == "todo");
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", unowned)) == "doing");
}

TEST_CASE("reconcile refuses to reset a task a LIVE claim now holds", "[agentactivity]") {
  // The `not exists (... status='active' and lease unexpired)` guard. Take
  // it away and a takeover's new owner has its task reset out from under
  // it the next time anyone sweeps.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto expired     = basic_args(fx, task);
  expired.ttl_secs = -60;
  auto const dead  = aa::acquire_claim(conn, expired);
  REQUIRE(dead.has_value());
  REQUIRE(aa::start_action(conn, aa::start_action_args{.session_id = fx.session_id,
                                                       .claim_id   = dead->id,
                                                       .kind       = aa::action_kind::coder,
                                                       .vendor     = "test"})
              .has_value());
  exec(conn, std::format("update tasks set status = 'doing' where id = {}", task));

  // A replacement claim takes over.
  auto forced  = basic_args(fx, task);
  forced.force = true;
  REQUIRE(aa::acquire_claim(conn, forced).has_value());

  REQUIRE(aa::reconcile_stale(conn, aa::reconcile_policy{}).has_value());
  REQUIRE(scalar_text(conn, std::format("select status from tasks where id = {}", task)) == "doing");
}

TEST_CASE("reconcile --plan scopes the sweep to one plan's claims", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  exec(conn, "insert into plans (scope_kind, title, slug) values ('global','other','other-plan')");
  auto const other_plan = scalar_int(conn, "select id from plans where slug = 'other-plan'");
  exec(conn,
       std::format("insert into tasks (scope_kind, plan_id, title, status) values ('global', {}, 'o', 'todo')", other_plan));
  auto const other_task = scalar_int(conn, std::format("select id from tasks where plan_id = {}", other_plan));

  auto mine          = basic_args(fx, task_id_at(conn, 0));
  mine.ttl_secs      = -60;
  auto const in_plan = aa::acquire_claim(conn, mine);
  REQUIRE(in_plan.has_value());

  auto theirs            = basic_args(fx, other_task);
  theirs.ttl_secs        = -60;
  auto const out_of_plan = aa::acquire_claim(conn, theirs);
  REQUIRE(out_of_plan.has_value());

  auto const swept = aa::reconcile_stale(conn, aa::reconcile_policy{.plan_id = fx.plan_id});
  REQUIRE(swept.has_value());
  REQUIRE(swept->claims_marked_stale == 1);
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", in_plan->id)) == "stale");
  // The other plan's expired claim shares the expiry predicate and is
  // still NOT swept — the plan-scoped path updates by ID for exactly this
  // reason rather than reusing the bulk predicate.
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", out_of_plan->id)) == "active");
}

TEST_CASE("reconcile --session scopes the sweep to one session's claims", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 2);
  exec(conn, "insert into sessions (vendor, vendor_session_id) values ('test', 'other')");
  auto const other_session = scalar_int(conn, "select id from sessions where vendor_session_id = 'other'");

  auto mine       = basic_args(fx, task_id_at(conn, 0));
  mine.ttl_secs   = -60;
  auto const ours = aa::acquire_claim(conn, mine);
  REQUIRE(ours.has_value());

  auto theirs        = basic_args(fx, task_id_at(conn, 1));
  theirs.ttl_secs    = -60;
  theirs.session_id  = other_session;
  auto const foreign = aa::acquire_claim(conn, theirs);
  REQUIRE(foreign.has_value());

  auto const swept = aa::reconcile_stale(conn, aa::reconcile_policy{.session_id = fx.session_id});
  REQUIRE(swept.has_value());
  REQUIRE(swept->claims_marked_stale == 1);
  REQUIRE(scalar_text(conn, std::format("select status from agent_work_claims where id = {}", foreign->id)) == "active");
}

TEST_CASE("reconcile closes actions orphaned by an ENDED session", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      held = aa::acquire_claim(conn, basic_args(fx, task_id_at(conn, 0)));
  REQUIRE(held.has_value());
  auto const opened = aa::start_action(
      conn,
      aa::start_action_args{.session_id = fx.session_id, .claim_id = held->id, .kind = aa::action_kind::coder, .vendor = "test"});
  REQUIRE(opened.has_value());

  // While the session is LIVE the action is not orphaned.
  REQUIRE(aa::reconcile_stale(conn, aa::reconcile_policy{}).has_value());
  REQUIRE(scalar_int(conn, std::format("select count(*) from agent_actions where id = {} and ended_at is null", *opened)) == 1);

  exec(conn, std::format("update sessions set ended_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = {}", fx.session_id));
  auto const swept = aa::reconcile_stale(conn, aa::reconcile_policy{});
  REQUIRE(swept.has_value());
  REQUIRE(swept->actions_closed >= 1);
  // Closed with the SESSION's ended_at, not with now, so the timeline does
  // not claim work continued past the session doing it.
  REQUIRE(scalar_text(conn, std::format("select outcome from agent_actions where id = {}", *opened)) == "aborted");
  REQUIRE(scalar_text(conn, std::format("select ended_at from agent_actions where id = {}", *opened)) ==
          scalar_text(conn, std::format("select ended_at from sessions where id = {}", fx.session_id)));
}

// ===========================================================================
// run sweep
// ===========================================================================

TEST_CASE("pid_alive treats non-positive pids as dead and self as alive", "[agentactivity]") {
  REQUIRE_FALSE(aa::pid_alive(0));
  REQUIRE_FALSE(aa::pid_alive(-1));
  // This process definitely exists.
  REQUIRE(aa::pid_alive(static_cast<std::int64_t>(::getpid())));
}

TEST_CASE("reconcile_runs abandons dead-pid runs and spares live ones", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);

  exec(conn, std::format("insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) "
                         "values ({}, 'wf', 'dead', -1, '/tmp')",
                         fx.plan_id));
  exec(conn, std::format("insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root) "
                         "values ({}, 'wf', 'live', {}, '/tmp')",
                         fx.plan_id, static_cast<std::int64_t>(::getpid())));

  auto const preview = aa::reconcile_runs(conn, true, std::nullopt);
  REQUIRE(preview.has_value());
  REQUIRE(preview->candidates.size() == 1);
  REQUIRE(preview->candidates[0].run_identifier == "dead");
  REQUIRE(preview->abandoned == 0);
  REQUIRE(scalar_text(conn, "select status from workflow_runs where run_identifier = 'dead'") == "running");

  auto const swept = aa::reconcile_runs(conn, false, std::nullopt);
  REQUIRE(swept.has_value());
  REQUIRE(swept->abandoned == 1);
  REQUIRE(scalar_text(conn, "select status from workflow_runs where run_identifier = 'dead'") == "abandoned");
  REQUIRE(scalar_text(conn, "select status from workflow_runs where run_identifier = 'live'") == "running");
}

// ===========================================================================
// reads
// ===========================================================================

TEST_CASE("get_task returns the fourteen rendered columns", "[agentactivity]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  auto const      task = task_id_at(conn, 0);

  auto const row = aa::get_task(conn, task);
  REQUIRE(row.has_value());
  REQUIRE(row->id == task);
  REQUIRE(row->plan_id == fx.plan_id);
  REQUIRE(row->status == "todo");
  REQUIRE(row->scope_kind == "global");
  REQUIRE_FALSE(row->body.has_value());

  REQUIRE(aa::get_task(conn, 99999).error() == aa::agent_error::task_not_found);
  REQUIRE(aa::current_task_status(conn, 99999).error() == aa::agent_error::task_not_found);
  REQUIRE_FALSE(aa::task_plan_id(conn, 99999).has_value());
}

TEST_CASE("a worktree id passes through opaquely while no worktrees table exists", "[agentactivity]") {
  // WRITTEN AS A REFUSAL FIRST, and the oracle corrected it. There is no
  // `worktrees` table at schema 33 — `sqlite3 planar.db .tables | grep
  // worktree` finds nothing — and `worktree_id` is deliberately NOT a
  // foreign key, so `validate_worktree_id`'s tolerant branch is the only
  // one currently reachable and an arbitrary id is stored verbatim.
  // Confirmed against the reference binary:
  //
  //     planar-agent claim --entity task:3 --worktree 4242
  //       -> exit 0, agent_work_claims.worktree_id = 4242
  //
  // The refusal path is still implemented (a database that grows the table
  // gets the check for free); it simply has no schema to fire against
  // today, and asserting a refusal here would have pinned a behaviour the
  // binary does not have.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      fx   = seed(conn, 1);
  REQUIRE(scalar_int(conn, "select count(*) from sqlite_master where type='table' and name='worktrees'") == 0);

  auto args        = basic_args(fx, task_id_at(conn, 0));
  args.worktree_id = 4242;
  auto const held  = aa::acquire_claim(conn, args);
  REQUIRE(held.has_value());
  REQUIRE(held->worktree_id == 4242);
}
