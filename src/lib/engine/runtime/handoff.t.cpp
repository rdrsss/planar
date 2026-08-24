// @file handoff.t.cpp
// @brief Unit tests for `planar.engine.runtime.handoff` (plan 996, task
// 6040).
//
// ORACLE PROVENANCE. Every rendered byte, every field order and every
// refusal below was captured by RUNNING the Zig binary in a pinned scratch
// arena (`cd` FIRST, then `env`), never from `--help`:
//
//   $Z init ; $Z assoc create project:proj --kind project
//   $Z assoc add project:proj $W/proj ; $Z plan create "Demo plan"
//   $Z task add "T2" --plan 1 --next-action "do the thing"
//   $Z capture session ; $Z handoff 2 --json
//     -> {"ok":true,"snapshot_id":4,"handoff_id":1,"status":"validated",
//         "resumable":true,"failures":[]}
//
//   $Z handoff show 1 --json
//     -> {"id":1,"from_snapshot_id":4,"from_vendor":"cli",
//         "status":"validated","created_at":"...","validated_at":"..."}
//        FIELD ORDER: the five always-present keys first, then optionals.
//        An unset optional is OMITTED, never emitted as null.
//
//   $Z handoff list --json           -> ZERO BYTES  (!!)
//   $Z handoff list                  -> no handoffs
//   $Z handoff list --status validated --json  -> two NDJSON lines, id 2 then 1
//   $Z handoff list --status validated
//     -> id    snapshot  from-vendor  to-vendor    status
//        2     5         cli          -            validated
//        1     4         cli          -            validated
//
//   $Z handoff consume 1 --json      -> status consumed, consumed_at added
//   $Z handoff abandon 2 --reason x --json
//     -> status abandoned, validated_at RETAINED, no consumed_at
//   $Z handoff validate 1 (consumed) -> exit 1,
//        error: handoff 1 cannot transition to validated
//   $Z handoff consume 1 (consumed)  -> exit 1,
//        error: handoff 1 is terminal; cannot consume
//
// THE ZERO-BYTE LIST IS THE FINDING WORTH NAMING. `handoff list --json`
// printed nothing on a database that HELD TWO HANDOFFS, because the
// default status filter is `{pending}` and both were `validated`. An
// implementation that defaulted to "all" would look correct in every
// casual check and be wrong here.
//
// A SECOND ONE: re-validating an ALREADY-validated handoff SUCCEEDS and
// re-stamps `validated_at` with a later timestamp. The matrix treats
// `from == to` as an identity no-op, and the UPDATE then runs anyway.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.transitions;
import planar.engine.runtime.handoff;

namespace {

namespace ho   = planar::engine::runtime::handoff;
namespace plan = planar::engine::planning;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_handoff_test_{}_{}.db",
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

auto new_session(planar::db::connection& conn) -> std::int64_t {
  exec(conn, "insert into sessions (vendor) values ('cli')");
  return scalar_int(conn, "select max(id) from sessions");
}

auto new_task(planar::db::connection& conn, std::string_view title) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, title, status, priority) values ('global', '{}', 'todo', 100)", title));
  return scalar_int(conn, std::format("select id from tasks where title = '{}'", title));
}

/// @brief A `context_snapshots` row to anchor a handoff on.
auto new_snapshot(planar::db::connection& conn, std::int64_t session_id, std::optional<std::int64_t> task_id) -> std::int64_t {
  if (task_id.has_value()) {
    exec(conn,
         std::format("insert into context_snapshots (session_id, task_id, vendor) values ({}, {}, 'cli')", session_id, *task_id));
  } else {
    exec(conn, std::format("insert into context_snapshots (session_id, vendor) values ({}, 'cli')", session_id));
  }
  return scalar_int(conn, "select max(id) from context_snapshots");
}

/// @brief The real guard the binary injects.
auto real_guard() -> ho::transition_check {
  return [](ho::status from, ho::status to) {
    return plan::check_transition(plan::transition_kind::handoff, ho::to_text(from), ho::to_text(to), false).has_value();
  };
}

} // namespace

TEST_CASE("handoff create: stores pending and normalizes empty optionals to NULL", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);

  // An EMPTY `to_vendor` must store SQL NULL, not an empty string — the
  // Zig insert's `if (v.len > 0)` guard. Otherwise `render_json` would
  // emit `"to_vendor":""` where the oracle omits the key entirely.
  auto const created = ho::create(conn, ho::create_args{.from_snapshot_id = snap,
                                                        .from_vendor      = "cli",
                                                        .to_vendor        = std::string_view{""},
                                                        .worktree_path    = std::nullopt,
                                                        .repo_root        = std::nullopt,
                                                        .branch           = std::nullopt});
  REQUIRE(created.has_value());
  REQUIRE(created->state == ho::status::pending);
  REQUIRE(created->from_snapshot_id == snap);
  REQUIRE(created->from_vendor == "cli");
  REQUIRE_FALSE(created->to_vendor.has_value());
  REQUIRE_FALSE(created->validated_at.has_value());
  REQUIRE_FALSE(created->consumed_at.has_value());
  REQUIRE(scalar_int(conn, std::format("select to_vendor is null from handoffs where id = {}", created->id)) == 1);
}

TEST_CASE("handoff create: worktree columns round-trip", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);

  auto const created = ho::create(conn, ho::create_args{.from_snapshot_id = snap,
                                                        .from_vendor      = "claude",
                                                        .to_vendor        = std::string_view{"codex"},
                                                        .worktree_path    = std::string_view{"/tmp/wt"},
                                                        .repo_root        = std::string_view{"/tmp/repo"},
                                                        .branch           = std::string_view{"feature/x"}});
  REQUIRE(created.has_value());
  REQUIRE(created->worktree_path == "/tmp/wt");
  REQUIRE(created->repo_root == "/tmp/repo");
  REQUIRE(created->branch == "feature/x");
  REQUIRE(created->to_vendor == "codex");
}

TEST_CASE("handoff show: absent id is not_found", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const found = ho::show(conn, 999);
  REQUIRE_FALSE(found.has_value());
  REQUIRE(found.error() == ho::handoff_error::not_found);
}

TEST_CASE("handoff transitions: the guard runs BEFORE the update", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());

  // A RECORDING guard that always REFUSES. If the implementation applied
  // the UPDATE first and consulted the matrix afterwards, the row would
  // have moved anyway and this test would still see `illegal_transition`.
  // Asserting the row is UNCHANGED is what makes the ordering observable.
  std::vector<std::pair<ho::status, ho::status>> seen;
  ho::transition_check                           refusing = [&seen](ho::status from, ho::status to) {
    seen.emplace_back(from, to);
    return false;
  };

  auto const refused = ho::validate(conn, created->id, refusing);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == ho::handoff_error::illegal_transition);
  REQUIRE(seen.size() == 1);
  REQUIRE(seen[0].first == ho::status::pending);
  REQUIRE(seen[0].second == ho::status::validated);

  auto const unchanged = ho::show(conn, created->id);
  REQUIRE(unchanged.has_value());
  REQUIRE(unchanged->state == ho::status::pending);
  REQUIRE_FALSE(unchanged->validated_at.has_value());
}

TEST_CASE("handoff validate: pending to validated stamps validated_at", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());

  auto const validated = ho::validate(conn, created->id, real_guard());
  REQUIRE(validated.has_value());
  REQUIRE(validated->state == ho::status::validated);
  REQUIRE(validated->validated_at.has_value());
  REQUIRE_FALSE(validated->consumed_at.has_value());
}

TEST_CASE("handoff validate: re-validating an already-validated handoff SUCCEEDS", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());

  auto const first = ho::validate(conn, created->id, real_guard());
  REQUIRE(first.has_value());

  // Identity transition: the matrix short-circuits `from == to`, and the
  // UPDATE runs anyway. Oracle-confirmed — a second `handoff validate 1`
  // exits 0 with a LATER validated_at.
  auto const second = ho::validate(conn, created->id, real_guard());
  REQUIRE(second.has_value());
  REQUIRE(second->state == ho::status::validated);
  REQUIRE(second->validated_at.has_value());
}

TEST_CASE("handoff consume: --session binds to_session_id, absent leaves it alone", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      bound   = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  auto const      unbound = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(bound.has_value());
  REQUIRE(unbound.has_value());

  auto const with_session = ho::consume(conn, bound->id, session, real_guard());
  REQUIRE(with_session.has_value());
  REQUIRE(with_session->state == ho::status::consumed);
  REQUIRE(with_session->to_session_id == session);
  REQUIRE(with_session->consumed_at.has_value());

  // No `--session`: the column must be left UNTOUCHED, not overwritten
  // with NULL. Two separate statements exist in the implementation for
  // exactly this.
  auto const without_session = ho::consume(conn, unbound->id, std::nullopt, real_guard());
  REQUIRE(without_session.has_value());
  REQUIRE(without_session->state == ho::status::consumed);
  REQUIRE_FALSE(without_session->to_session_id.has_value());
}

TEST_CASE("handoff abandon: retains validated_at and leaves consumed_at unset", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());
  auto const validated = ho::validate(conn, created->id, real_guard());
  REQUIRE(validated.has_value());

  auto const abandoned = ho::abandon(conn, created->id, real_guard());
  REQUIRE(abandoned.has_value());
  REQUIRE(abandoned->state == ho::status::abandoned);
  // Oracle-captured: abandoning a VALIDATED handoff keeps its
  // validated_at. Only `status` moves.
  REQUIRE(abandoned->validated_at == validated->validated_at);
  REQUIRE_FALSE(abandoned->consumed_at.has_value());
}

TEST_CASE("handoff transitions: both terminal states refuse every outgoing edge", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);

  for (auto const terminal : {ho::status::consumed, ho::status::abandoned}) {
    auto const created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
    REQUIRE(created.has_value());
    if (terminal == ho::status::consumed) {
      REQUIRE(ho::consume(conn, created->id, std::nullopt, real_guard()).has_value());
    } else {
      REQUIRE(ho::abandon(conn, created->id, real_guard()).has_value());
    }

    // `validate` and, for each, the OTHER terminal move are all refused.
    auto const revalidate = ho::validate(conn, created->id, real_guard());
    REQUIRE_FALSE(revalidate.has_value());
    REQUIRE(revalidate.error() == ho::handoff_error::illegal_transition);

    if (terminal == ho::status::consumed) {
      auto const abandoning = ho::abandon(conn, created->id, real_guard());
      REQUIRE_FALSE(abandoning.has_value());
      REQUIRE(abandoning.error() == ho::handoff_error::illegal_transition);
    } else {
      auto const consuming = ho::consume(conn, created->id, std::nullopt, real_guard());
      REQUIRE_FALSE(consuming.has_value());
      REQUIRE(consuming.error() == ho::handoff_error::illegal_transition);
    }
  }
}

TEST_CASE("handoff transitions: consuming a still-PENDING handoff is legal", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());

  // pending -> consumed skips validation entirely, and the matrix allows
  // it. A matrix that only permitted validated -> consumed would break
  // this path.
  auto const consumed = ho::consume(conn, created->id, std::nullopt, real_guard());
  REQUIRE(consumed.has_value());
  REQUIRE(consumed->state == ho::status::consumed);
}

TEST_CASE("handoff list: an EMPTY status filter means pending, not everything", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  auto const      session  = new_session(conn);
  auto const      snap     = new_snapshot(conn, session, std::nullopt);
  auto const      pending  = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  auto const      becoming = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(pending.has_value());
  REQUIRE(becoming.has_value());
  REQUIRE(ho::validate(conn, becoming->id, real_guard()).has_value());

  // THE FINDING: the default filter is {pending}. Two handoffs exist; the
  // default list returns ONE.
  auto const defaulted = ho::list(conn, ho::list_filter{});
  REQUIRE(defaulted.has_value());
  REQUIRE(defaulted->size() == 1);
  REQUIRE((*defaulted)[0].id == pending->id);

  auto const validated_only = ho::list(conn, ho::list_filter{.statuses = {ho::status::validated}});
  REQUIRE(validated_only.has_value());
  REQUIRE(validated_only->size() == 1);
  REQUIRE((*validated_only)[0].id == becoming->id);

  auto const both = ho::list(conn, ho::list_filter{.statuses = {ho::status::pending, ho::status::validated}});
  REQUIRE(both.has_value());
  REQUIRE(both->size() == 2);
}

TEST_CASE("handoff list: task_id joins through the snapshot", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      task_a  = new_task(conn, "Task A");
  auto const      task_b  = new_task(conn, "Task B");
  auto const      snap_a  = new_snapshot(conn, session, task_a);
  auto const      snap_b  = new_snapshot(conn, session, task_b);

  auto const on_a = ho::create(conn, ho::create_args{.from_snapshot_id = snap_a, .from_vendor = "cli"});
  REQUIRE(on_a.has_value());
  REQUIRE(ho::create(conn, ho::create_args{.from_snapshot_id = snap_b, .from_vendor = "cli"}).has_value());

  auto const only_a = ho::list(conn, ho::list_filter{.task_id = task_a});
  REQUIRE(only_a.has_value());
  REQUIRE(only_a->size() == 1);
  REQUIRE((*only_a)[0].id == on_a->id);
}

TEST_CASE("handoff get_latest_with_worktree_for_task: skips NULL worktrees and abandoned rows", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      task    = new_task(conn, "Task A");
  auto const      snap    = new_snapshot(conn, session, task);

  // No worktree at all -> not a candidate.
  REQUIRE(ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"}).has_value());
  REQUIRE_FALSE(ho::get_latest_with_worktree_for_task(conn, task)->has_value());

  // A worktree-bearing but ABANDONED row -> still not a candidate.
  auto const abandoned = ho::create(
      conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli", .worktree_path = std::string_view{"/tmp/dead"}});
  REQUIRE(abandoned.has_value());
  REQUIRE(ho::abandon(conn, abandoned->id, real_guard()).has_value());
  REQUIRE_FALSE(ho::get_latest_with_worktree_for_task(conn, task)->has_value());

  // A CONSUMED one IS a candidate — the filter is deliberately permissive
  // because a consumed handoff still carries the prior cycle's worktree.
  auto const consumed = ho::create(
      conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli", .worktree_path = std::string_view{"/tmp/live"}});
  REQUIRE(consumed.has_value());
  REQUIRE(ho::consume(conn, consumed->id, std::nullopt, real_guard()).has_value());
  auto const found = ho::get_latest_with_worktree_for_task(conn, task);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  REQUIRE((*found)->worktree_path == "/tmp/live");
}

TEST_CASE("handoff get_pending_for_snapshot: pending and validated only", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);

  REQUIRE_FALSE(ho::get_pending_for_snapshot(conn, snap)->has_value());

  auto const created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());
  REQUIRE(ho::get_pending_for_snapshot(conn, snap)->has_value());

  REQUIRE(ho::consume(conn, created->id, std::nullopt, real_guard()).has_value());
  // Consumed drops out of the "open" set.
  REQUIRE_FALSE(ho::get_pending_for_snapshot(conn, snap)->has_value());
}

TEST_CASE("handoff render_json: field order is fixed and unset optionals are OMITTED", "[engine_runtime][handoff]") {
  ho::handoff minimal{
      .id               = 1,
      .from_snapshot_id = 4,
      .to_session_id    = std::nullopt,
      .from_vendor      = "cli",
      .to_vendor        = std::nullopt,
      .state            = ho::status::validated,
      .validated_at     = "2026-08-24T00:45:33.560Z",
      .consumed_at      = std::nullopt,
      .created_at       = "2026-08-24T00:45:33.482Z",
      .worktree_path    = std::nullopt,
      .repo_root        = std::nullopt,
      .branch           = std::nullopt,
  };
  // The exact oracle bytes, and NO trailing newline — this renderer is a
  // FRAGMENT so `render_list_json` can compose NDJSON from it.
  REQUIRE(ho::render_json(minimal) == "{\"id\":1,\"from_snapshot_id\":4,\"from_vendor\":\"cli\","
                                      "\"status\":\"validated\",\"created_at\":\"2026-08-24T00:45:33.482Z\","
                                      "\"validated_at\":\"2026-08-24T00:45:33.560Z\"}");
  REQUIRE(ho::render_json(minimal).find("null") == std::string::npos);
  REQUIRE_FALSE(ho::render_json(minimal).ends_with("\n"));

  ho::handoff full   = minimal;
  full.to_session_id = 7;
  full.to_vendor     = "codex";
  full.consumed_at   = "2026-08-24T00:45:40.932Z";
  full.worktree_path = "/tmp/wt";
  full.repo_root     = "/tmp/repo";
  full.branch        = "feature/x";
  // Optional ORDER: to_session_id, to_vendor, validated_at, consumed_at,
  // worktree_path, repo_root, branch.
  REQUIRE(ho::render_json(full) == "{\"id\":1,\"from_snapshot_id\":4,\"from_vendor\":\"cli\","
                                   "\"status\":\"validated\",\"created_at\":\"2026-08-24T00:45:33.482Z\","
                                   "\"to_session_id\":7,\"to_vendor\":\"codex\","
                                   "\"validated_at\":\"2026-08-24T00:45:33.560Z\","
                                   "\"consumed_at\":\"2026-08-24T00:45:40.932Z\","
                                   "\"worktree_path\":\"/tmp/wt\",\"repo_root\":\"/tmp/repo\","
                                   "\"branch\":\"feature/x\"}");
}

TEST_CASE("handoff render_json: escapes text through json_text", "[engine_runtime][handoff]") {
  ho::handoff quoted{
      .id               = 1,
      .from_snapshot_id = 1,
      .to_session_id    = std::nullopt,
      .from_vendor      = "he said \"hi\"\\",
      .to_vendor        = std::nullopt,
      .state            = ho::status::pending,
      .validated_at     = std::nullopt,
      .consumed_at      = std::nullopt,
      .created_at       = "T",
      .worktree_path    = std::nullopt,
      .repo_root        = std::nullopt,
      .branch           = std::nullopt,
  };
  REQUIRE(ho::render_json(quoted).find("\"from_vendor\":\"he said \\\"hi\\\"\\\\\"") != std::string::npos);
}

TEST_CASE("handoff render_list_json: empty input is ZERO BYTES", "[engine_runtime][handoff]") {
  std::vector<ho::handoff> none;
  // Not `[]`, not a bare newline. The same complete-payload zero-byte case
  // `workflow list --json` pins.
  REQUIRE(ho::render_list_json(none).empty());
}

TEST_CASE("handoff render_list_json: one NDJSON line per handoff", "[engine_runtime][handoff]") {
  ho::handoff one{.id               = 2,
                  .from_snapshot_id = 5,
                  .to_session_id    = std::nullopt,
                  .from_vendor      = "cli",
                  .to_vendor        = std::nullopt,
                  .state            = ho::status::validated,
                  .validated_at     = std::nullopt,
                  .consumed_at      = std::nullopt,
                  .created_at       = "T2",
                  .worktree_path    = std::nullopt,
                  .repo_root        = std::nullopt,
                  .branch           = std::nullopt};
  ho::handoff two = one;
  two.id          = 1;
  two.created_at  = "T1";

  std::vector<ho::handoff> items{one, two};
  auto const               rendered = ho::render_list_json(items);
  REQUIRE(rendered == ho::render_json(one) + "\n" + ho::render_json(two) + "\n");
  REQUIRE(std::ranges::count(rendered, '\n') == 2);
}

TEST_CASE("handoff render_list_text: empty says so; rows pad to the oracle's widths", "[engine_runtime][handoff]") {
  std::vector<ho::handoff> none;
  REQUIRE(ho::render_list_text(none) == "no handoffs\n");

  ho::handoff              row{.id               = 2,
                               .from_snapshot_id = 5,
                               .to_session_id    = std::nullopt,
                               .from_vendor      = "cli",
                               .to_vendor        = std::nullopt,
                               .state            = ho::status::validated,
                               .validated_at     = std::nullopt,
                               .consumed_at      = std::nullopt,
                               .created_at       = "T",
                               .worktree_path    = std::nullopt,
                               .repo_root        = std::nullopt,
                               .branch           = std::nullopt};
  std::vector<ho::handoff> items{row};
  // Byte-exact against the captured oracle table, `-` standing in for the
  // unset to_vendor.
  REQUIRE(ho::render_list_text(items) == "id    snapshot  from-vendor  to-vendor    status\n"
                                         "2     5         cli          -            validated\n");
}

TEST_CASE("handoff render_text: optional lines appear only when set", "[engine_runtime][handoff]") {
  ho::handoff minimal{.id               = 1,
                      .from_snapshot_id = 4,
                      .to_session_id    = std::nullopt,
                      .from_vendor      = "cli",
                      .to_vendor        = std::nullopt,
                      .state            = ho::status::pending,
                      .validated_at     = std::nullopt,
                      .consumed_at      = std::nullopt,
                      .created_at       = "T",
                      .worktree_path    = std::nullopt,
                      .repo_root        = std::nullopt,
                      .branch           = std::nullopt};
  REQUIRE(ho::render_text(minimal) == "handoff 1  [pending]\n"
                                      "  from_snapshot: 4\n"
                                      "  from_vendor:   cli\n"
                                      "  created_at:    T\n");

  ho::handoff full   = minimal;
  full.to_vendor     = "codex";
  full.to_session_id = 3;
  full.validated_at  = "V";
  full.consumed_at   = "C";
  full.worktree_path = "/tmp/wt";
  full.repo_root     = "/tmp/repo";
  full.branch        = "b";
  full.state         = ho::status::consumed;
  // `created_at` sits AFTER the timestamps and BEFORE the worktree block.
  REQUIRE(ho::render_text(full) == "handoff 1  [consumed]\n"
                                   "  from_snapshot: 4\n"
                                   "  from_vendor:   cli\n"
                                   "  to_vendor:     codex\n"
                                   "  to_session:    3\n"
                                   "  validated_at:  V\n"
                                   "  consumed_at:   C\n"
                                   "  created_at:    T\n"
                                   "  worktree_path: /tmp/wt\n"
                                   "  repo_root:     /tmp/repo\n"
                                   "  branch:        b\n");
}

TEST_CASE("handoff status text round-trips and rejects anything else", "[engine_runtime][handoff]") {
  for (auto const value : {ho::status::pending, ho::status::validated, ho::status::consumed, ho::status::abandoned}) {
    REQUIRE(ho::status_from_text(ho::to_text(value)) == value);
  }
  REQUIRE_FALSE(ho::status_from_text("").has_value());
  REQUIRE_FALSE(ho::status_from_text("Pending").has_value());
  REQUIRE_FALSE(ho::status_from_text("validated ").has_value());

  REQUIRE(ho::is_terminal(ho::status::consumed));
  REQUIRE(ho::is_terminal(ho::status::abandoned));
  REQUIRE_FALSE(ho::is_terminal(ho::status::pending));
  REQUIRE_FALSE(ho::is_terminal(ho::status::validated));
}

TEST_CASE("handoff read: an out-of-matrix status text is query_failed", "[engine_runtime][handoff]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      session = new_session(conn);
  auto const      snap    = new_snapshot(conn, session, std::nullopt);
  auto const      created = ho::create(conn, ho::create_args{.from_snapshot_id = snap, .from_vendor = "cli"});
  REQUIRE(created.has_value());

  // The column is CHECK-constrained, so an out-of-matrix value can only be
  // written with the constraint suspended. `ignore_check_constraints` is
  // the purpose-built pragma for it and leaves the schema itself intact,
  // unlike a `writable_schema` rewrite. Reaching this state proves the
  // blunt mapping the Zig original chose (`orelse return
  // Error.QueryFailed`) is reproduced, rather than the row silently
  // reading back as `pending`.
  exec(conn, "pragma ignore_check_constraints = on");
  exec(conn, std::format("update handoffs set status = 'bogus' where id = {}", created->id));
  exec(conn, "pragma ignore_check_constraints = off");
  REQUIRE(scalar_int(conn, std::format("select count(*) from handoffs where id = {} and status = 'bogus'", created->id)) == 1);

  auto const found = ho::show(conn, created->id);
  REQUIRE_FALSE(found.has_value());
  REQUIRE(found.error() == ho::handoff_error::query_failed);
}
