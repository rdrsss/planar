// @file lifecycle.t.cpp
// @brief Unit tests for `planar.engine.runs.lifecycle` (plan 996, task 6095).
//
// ORACLE PROVENANCE. Every row shape, ordering, and error outcome below was
// captured by RUNNING the Zig binary against a scratch database, never from
// `--help` and never from reading the Zig source:
//
//   mkdir -p /tmp/orc/repo && cd /tmp/orc/repo && git init -q .
//   export PLANAR_DB=/tmp/orc/p.db PLANAR_CONFIG_PATH=/tmp/orc/p.toml
//   Z=./zig/zig-out/bin/planar
//   $Z init
//   $Z assoc create project:repo --kind project
//   $Z assoc add project:repo /tmp/orc/repo
//   $Z plan create "Bench Plan" --slug bench-plan          -> id 1
//   $Z task add "T one" --plan 1 --slug t-one              -> id 1
//   $Z task add "T two" --plan 1 --slug t-two              -> id 2
//   $Z task touches add 1 repo --path src/a.zig
//   $Z task touches add 2 repo --path src/b.zig
//
// --- start / duplicate uid ------------------------------------------------
//   $Z bench start r1 --plan 1 --arm strict --base-sha deadbeef \
//        --config-hash ch1                     -> exit 0, stdout "r1"
//   $Z bench start r1 ... (again)              -> exit 6
//        stderr: error: bench start: run_uid 'r1' already exists
//   $Z bench start r2 --plan 999 ...           -> exit 1
//        stderr: error: bench start: QueryFailed      [FK violation]
//
// --- HAZARD 3: the declared-touch snapshot at start -----------------------
// The brief did not mention this; it was found by probing. `start` copies
// task_touch_paths into run_touches as kind='declared', in the same txn.
//
//   $Z bench start r10 --plan 1 --arm strict --base-sha s --config-hash c
//   $Z bench show r10 --json
//     -> "touches":[{"id":4,"task_id":1,"path":"src/a.zig","kind":"declared",..},
//                   {"id":5,"task_id":2,"path":"src/b.zig","kind":"declared",..}]
//
//   $Z bench start r9 ... --task 1             [FILTERED to one task]
//   $Z bench show r9 --json
//     -> "touches":[{"id":3,"task_id":1,"path":"src/a.zig","kind":"declared",..}]
//
//   $Z task update 2 --status doing ; $Z task done 2
//   $Z bench start r11 --plan 1 --arm strict --base-sha s --config-hash c
//   sqlite3 p.db 'select task_id,path from run_touches where run_id=9'
//       1|src/a.zig
//       2|src/b.zig                <-- task 2 is `done` and STILL snapshotted
//
// --- HAZARD 3: out-of-order lifecycle calls are NOT guarded ---------------
//   $Z bench finish r1 --status completed   -> exit 0, stdout "ok"
//   $Z bench finish r1 --status aborted     -> exit 0  [second finish WINS,
//                                                       status and ended_at
//                                                       both rewritten]
//   $Z bench event r1 --kind postfinish --seq 20 -> exit 0  [event AFTER finish]
//   $Z bench touch r1 --task 1 --path src/b.zig --kind actual -> exit 0
//   $Z bench finish nope --status completed -> exit 1
//        stderr: error: bench finish: run 'nope' not found
//   $Z bench event nope --kind x --seq 1    -> exit 1  [run 'nope' not found]
//   $Z bench touch nope --task 1 --path p --kind actual -> exit 1  [same]
//   $Z run finish <uid> --status completed  -> exit 0, repeatable
//   $Z run event <uid> --kind postfinish    -> exit 0, seq 4  [after finish]
//
// --- duplicate seq / duplicate touch --------------------------------------
//   $Z bench event r1 --kind dispatch --seq 1  (twice) -> exit 6
//        stderr: error: bench event: seq 1 already used for run 'r1'
//   $Z bench touch r1 --task 1 --path src/a.zig --kind declared (twice)
//        -> exit 1, stderr: error: bench touch: QueryFailed
//
// --- HAZARD 4: event ordering is by seq, not by insertion id --------------
//   seq 1, then seq 2, then seq 9 (inserted third):
//   $Z bench show r1
//       events (3):
//         [1] dispatch
//         [2] result: {"a":1}
//         [9] after
//
// --- run start: same `runs` table, arm 'op', empty base_sha/config_hash ---
//   $Z run start --plan 1 --json
//       -> {"run_uid":"c5678087d1831bc7fe1e47a35d35f5f3","plan_id":1,"arm":"op"}
//   $Z run start --plan 1 --workflow wf1 --json         -> "arm":"wf1"
//   sqlite3 p.db 'select id,run_uid,arm,base_sha,config_hash from runs'
//       3|c5678087d1831bc7fe1e47a35d35f5f3|op||
//       5|1dab822ffd79ff9e6463728b3fb6dfbb|wf1||
//
// --- run event: seq auto-increments ---------------------------------------
//   $Z run event <uid> --kind step  -> {"run_uid":"...","seq":1,"kind":"step"}
//   $Z run event <uid> --kind step  -> ..."seq":2
//   $Z run event <uid> --kind step2 -> ..."seq":3
//
// `bench harvest` is NOT covered: it is not ported (see CMakeLists.txt's cut
// list -- it rests on git subprocess work). That is the one leaf of the ten
// this cycle does not reach.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runs.lifecycle;

namespace {

namespace rl = planar::engine::runs::lifecycle;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_runs_test_{}_{}.db",
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

auto is_null(planar::db::connection& conn, std::string_view sql) -> bool {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->is_null(0);
}

/// @brief Seed the fixture the oracle transcript above was captured against:
/// plan 1 with tasks 1 and 2, each declaring one path-level touch.
auto seed_plan_with_touches(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (slug, name, root_path) values ('repo', 'repo', '/tmp/orc/repo')");
  exec(conn, "insert into plans (scope_kind, title, slug, status) "
             "values ('global', 'Bench Plan', 'bench-plan', 'draft')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, slug, status) "
             "values ('global', 1, 'T one', 't-one', 'todo')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, slug, status) "
             "values ('global', 1, 'T two', 't-two', 'todo')");
  exec(conn, "insert into task_touch_paths (task_id, repo_id, path) values (1, 1, 'src/a.zig')");
  exec(conn, "insert into task_touch_paths (task_id, repo_id, path) values (2, 1, 'src/b.zig')");
}

auto seed_bare_plan(planar::db::connection& conn) -> void {
  exec(conn, "insert into plans (scope_kind, title, slug, status) "
             "values ('global', 'Bare', 'bare', 'draft')");
}

} // namespace

TEST_CASE("runs.lifecycle: touch_kind round-trips the schema's two-value set", "[runs]") {
  REQUIRE(rl::touch_kind_from_text("declared") == rl::touch_kind::declared);
  REQUIRE(rl::touch_kind_from_text("actual") == rl::touch_kind::actual);
  REQUIRE_FALSE(rl::touch_kind_from_text("bogus").has_value());
  REQUIRE_FALSE(rl::touch_kind_from_text("").has_value());
  // Case matters -- the schema CHECK is case-sensitive.
  REQUIRE_FALSE(rl::touch_kind_from_text("Declared").has_value());

  REQUIRE(rl::touch_kind_to_text(rl::touch_kind::declared) == "declared");
  REQUIRE(rl::touch_kind_to_text(rl::touch_kind::actual) == "actual");
}

TEST_CASE("runs.lifecycle: start inserts a run with the schema's defaults", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);

  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = "r1",
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "deadbeef",
                                     .config_hash = "ch1",
                                 });
  REQUIRE(started.has_value());
  REQUIRE(started->id == 1);
  REQUIRE(started->run_uid == "r1");
  // No task_touch_paths seeded, so nothing to snapshot.
  REQUIRE(started->declared_snapshotted == 0);

  // The oracle's post-state: status defaults to 'running', ended_at NULL,
  // config_json and corpus_repo NULL.
  REQUIRE(scalar_text(conn, "select status from runs where id = 1") == "running");
  REQUIRE(is_null(conn, "select ended_at from runs where id = 1"));
  REQUIRE(is_null(conn, "select config_json from runs where id = 1"));
  REQUIRE(is_null(conn, "select corpus_repo from runs where id = 1"));
  REQUIRE(scalar_text(conn, "select base_sha from runs where id = 1") == "deadbeef");
}

TEST_CASE("runs.lifecycle: start refuses a duplicate run_uid", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);

  const rl::start_args args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"};
  REQUIRE(rl::start(conn, args).has_value());

  auto second = rl::start(conn, args);
  REQUIRE_FALSE(second.has_value());
  // Distinguished from a generic failure -- the oracle prints a dedicated
  // "run_uid 'r1' already exists" at exit 6, not a bare QueryFailed.
  REQUIRE(second.error() == rl::runs_error::duplicate_run_uid);
  // The rollback left exactly one row.
  REQUIRE(scalar_int(conn, "select count(*) from runs") == 1);
}

TEST_CASE("runs.lifecycle: start on a missing plan fails and writes nothing", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);

  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r2", .plan_id = 999, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE_FALSE(started.has_value());
  // The oracle reports this as a bare `bench start: QueryFailed` at exit 1 --
  // an FK violation, not a uid collision.
  REQUIRE(started.error() == rl::runs_error::query_failed);
  REQUIRE(scalar_int(conn, "select count(*) from runs") == 0);
}

TEST_CASE("runs.lifecycle: start snapshots every plan task's declared touches", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan_with_touches(conn);

  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r10", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());
  REQUIRE(started->declared_snapshotted == 2);

  auto rows = rl::touches(conn, started->id, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  REQUIRE((*rows)[0].task_id == 1);
  REQUIRE((*rows)[0].path == "src/a.zig");
  REQUIRE((*rows)[0].kind_ == rl::touch_kind::declared);
  REQUIRE((*rows)[1].task_id == 2);
  REQUIRE((*rows)[1].path == "src/b.zig");
  REQUIRE((*rows)[1].kind_ == rl::touch_kind::declared);
}

TEST_CASE("runs.lifecycle: start snapshots tasks in a terminal status too", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan_with_touches(conn);
  // The oracle capture: task 2 was driven to `done` and its path STILL
  // landed in the next run's snapshot. This is the one place `runs` and
  // `groups recommend` genuinely disagree -- grouping filters to
  // status='todo', this does not filter at all.
  exec(conn, "update tasks set status = 'done' where id = 2");

  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r11", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());
  REQUIRE(started->declared_snapshotted == 2);

  auto rows = rl::touches(conn, started->id, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  REQUIRE((*rows)[1].task_id == 2);
  REQUIRE((*rows)[1].path == "src/b.zig");
}

TEST_CASE("runs.lifecycle: start's task filter narrows the snapshot to the listed ids", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan_with_touches(conn);

  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = "r9",
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "s",
                                     .config_hash = "c",
                                     .task_filter = std::vector<std::int64_t>{1},
                                 });
  REQUIRE(started.has_value());
  REQUIRE(started->declared_snapshotted == 1);

  auto rows = rl::touches(conn, started->id, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  REQUIRE((*rows)[0].task_id == 1);
  REQUIRE((*rows)[0].path == "src/a.zig");
}

TEST_CASE("runs.lifecycle: start's task filter still constrains to the plan", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan_with_touches(conn);
  // A second plan with its own task and its own declared touch. Naming that
  // foreign task in the filter must contribute nothing: the snapshot query
  // joins `tasks` and requires t.plan_id = the run's plan.
  exec(conn, "insert into plans (scope_kind, title, slug, status) "
             "values ('global', 'Other', 'other', 'draft')");
  exec(conn, "insert into tasks (scope_kind, plan_id, title, slug, status) "
             "values ('global', 2, 'Foreign', 'foreign', 'todo')");
  exec(conn, "insert into task_touch_paths (task_id, repo_id, path) values (3, 1, 'src/foreign.zig')");

  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = "rf",
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "s",
                                     .config_hash = "c",
                                     .task_filter = std::vector<std::int64_t>{1, 3},
                                 });
  REQUIRE(started.has_value());
  REQUIRE(started->declared_snapshotted == 1);

  auto rows = rl::touches(conn, started->id, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  REQUIRE((*rows)[0].task_id == 1);
}

TEST_CASE("runs.lifecycle: an empty task filter snapshots nothing", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan_with_touches(conn);
  // An engaged-but-empty filter is NOT the same as an absent filter. The
  // absent case snapshots every plan task (two rows); this must snapshot
  // zero. Both are reachable from `bench start` -- absent when --task is
  // never passed, empty only through this engine API.
  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = "re",
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "s",
                                     .config_hash = "c",
                                     .task_filter = std::vector<std::int64_t>{},
                                 });
  REQUIRE(started.has_value());
  REQUIRE(started->declared_snapshotted == 0);
  REQUIRE(scalar_int(conn, "select count(*) from run_touches") == 0);
}

TEST_CASE("runs.lifecycle: the declared snapshot is a value copy, not a live view", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_plan_with_touches(conn);

  auto started =
      rl::start(conn, rl::start_args{.run_uid = "rv", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  // Rewrite the declarations AFTER start. The recorded prediction must not
  // move -- that immutability is what the RQ1 experiment measures.
  exec(conn, "delete from task_touch_paths");
  exec(conn, "insert into task_touch_paths (task_id, repo_id, path) values (1, 1, 'src/rewritten.zig')");

  auto rows = rl::touches(conn, started->id, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  REQUIRE((*rows)[0].path == "src/a.zig");
  REQUIRE((*rows)[1].path == "src/b.zig");
}

TEST_CASE("runs.lifecycle: start stores an explicit initial status and the raw config blobs", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);

  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = "r3",
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "s",
                                     .config_hash = "c",
                                     .config_json = std::string_view{R"({"k":1})"},
                                     .corpus_repo = std::string_view{"/x"},
                                     .status      = std::string_view{"queued"},
                                 });
  REQUIRE(started.has_value());
  REQUIRE(scalar_text(conn, "select config_json from runs where id = 1") == R"({"k":1})");
  REQUIRE(scalar_text(conn, "select corpus_repo from runs where id = 1") == "/x");
  // `coalesce(?, 'running')` must yield the caller's value, not the default.
  REQUIRE(scalar_text(conn, "select status from runs where id = 1") == "queued");
}

TEST_CASE("runs.lifecycle: run start's empty base_sha and config_hash stay empty, not NULL", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);

  // This is `run start`'s exact shape (oracle: `select base_sha from runs`
  // on an op run yields '', and both columns are NOT NULL). It is also the
  // one path in this module where db::statement::bind_text's null-data_()
  // defect (task 6097) would bite: a default-constructed string_view binds
  // SQL NULL and the insert would fail the NOT NULL constraint outright.
  auto started = rl::start(
      conn,
      rl::start_args{
          .run_uid = "opuid", .plan_id = 1, .arm = "op", .base_sha = std::string_view{}, .config_hash = std::string_view{}});
  REQUIRE(started.has_value());
  REQUIRE_FALSE(is_null(conn, "select base_sha from runs where id = 1"));
  REQUIRE_FALSE(is_null(conn, "select config_hash from runs where id = 1"));
  REQUIRE(scalar_text(conn, "select base_sha from runs where id = 1").empty());
  REQUIRE(scalar_text(conn, "select config_hash from runs where id = 1").empty());
}

TEST_CASE("runs.lifecycle: event refuses a re-used seq for the same run", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  REQUIRE(rl::event(conn, started->id, 1, "dispatch", std::nullopt).has_value());

  auto dup = rl::event(conn, started->id, 1, "dispatch", std::nullopt);
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error() == rl::runs_error::duplicate_seq);
  REQUIRE(scalar_int(conn, "select count(*) from run_events") == 1);
}

TEST_CASE("runs.lifecycle: the same seq on a DIFFERENT run is legal", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto a = rl::start(conn, rl::start_args{.run_uid = "ra", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  auto b = rl::start(conn, rl::start_args{.run_uid = "rb", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  // UNIQUE is (run_id, seq), not (seq) -- a per-run ordinal, not a global one.
  REQUIRE(rl::event(conn, a->id, 1, "k", std::nullopt).has_value());
  REQUIRE(rl::event(conn, b->id, 1, "k", std::nullopt).has_value());
  REQUIRE(scalar_int(conn, "select count(*) from run_events") == 2);
}

TEST_CASE("runs.lifecycle: events come back ordered by seq, not by insertion id", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  // Insert deliberately out of seq order so id-order and seq-order disagree:
  // ids 1,2,3 carry seqs 9,1,5. The oracle renders [1] [5] [9].
  REQUIRE(rl::event(conn, started->id, 9, "third", std::nullopt).has_value());
  REQUIRE(rl::event(conn, started->id, 1, "first", std::nullopt).has_value());
  REQUIRE(rl::event(conn, started->id, 5, "second", std::nullopt).has_value());

  auto rows = rl::events(conn, started->id);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 3);
  REQUIRE((*rows)[0].seq == 1);
  REQUIRE((*rows)[0].kind == "first");
  REQUIRE((*rows)[1].seq == 5);
  REQUIRE((*rows)[1].kind == "second");
  REQUIRE((*rows)[2].seq == 9);
  REQUIRE((*rows)[2].kind == "third");
  // ...and the ids prove the order really is by seq: the seq-1 row is id 2.
  REQUIRE((*rows)[0].id == 2);
  REQUIRE((*rows)[2].id == 1);
}

TEST_CASE("runs.lifecycle: a payload round-trips verbatim, including whitespace", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  // Oracle-captured: `--payload '   {"a":1}   '` is accepted and stored with
  // its padding intact, which is visible in `bench show --json` as
  // `"payload":   {"a":1}   ,`.
  REQUIRE(rl::event(conn, started->id, 1, "padded", std::string_view{R"(   {"a":1}   )"}).has_value());
  REQUIRE(rl::event(conn, started->id, 2, "bare", std::nullopt).has_value());

  auto rows = rl::events(conn, started->id);
  REQUIRE(rows.has_value());
  REQUIRE((*rows)[0].payload.has_value());
  REQUIRE(*(*rows)[0].payload == R"(   {"a":1}   )");
  REQUIRE_FALSE((*rows)[1].payload.has_value());
}

TEST_CASE("runs.lifecycle: next_seq starts at 1 and tracks the maximum, not the count", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  auto first = rl::next_seq(conn, started->id);
  REQUIRE(first.has_value());
  REQUIRE(*first == 1);

  REQUIRE(rl::event(conn, started->id, 1, "a", std::nullopt).has_value());
  auto second = rl::next_seq(conn, started->id);
  REQUIRE(second.has_value());
  REQUIRE(*second == 2);

  // A caller-supplied gap must move next_seq to max+1, NOT to count+1 -- the
  // two differ the moment `bench event --seq 50` shares a run with
  // `run event`. count+1 would be 3 here and would collide on the next write.
  REQUIRE(rl::event(conn, started->id, 50, "b", std::nullopt).has_value());
  auto third = rl::next_seq(conn, started->id);
  REQUIRE(third.has_value());
  REQUIRE(*third == 51);
}

TEST_CASE("runs.lifecycle: next_seq is per-run", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto a = rl::start(conn, rl::start_args{.run_uid = "ra", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  auto b = rl::start(conn, rl::start_args{.run_uid = "rb", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(rl::event(conn, a->id, 7, "k", std::nullopt).has_value());

  auto other = rl::next_seq(conn, b->id);
  REQUIRE(other.has_value());
  REQUIRE(*other == 1);
}

TEST_CASE("runs.lifecycle: touch refuses a duplicate tuple, touch_idempotent absorbs it", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  REQUIRE(rl::touch(conn, started->id, 1, "src/a.zig", rl::touch_kind::declared).has_value());

  auto dup = rl::touch(conn, started->id, 1, "src/a.zig", rl::touch_kind::declared);
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error() == rl::runs_error::query_failed);

  // Same tuple through the idempotent path succeeds and adds no row -- this
  // is the primitive `bench harvest` writes through.
  REQUIRE(rl::touch_idempotent(conn, started->id, 1, "src/a.zig", rl::touch_kind::declared).has_value());
  REQUIRE(scalar_int(conn, "select count(*) from run_touches") == 1);

  // The UNIQUE tuple includes `kind`, so the same (run, task, path) under the
  // other kind is a genuinely new row.
  REQUIRE(rl::touch(conn, started->id, 1, "src/a.zig", rl::touch_kind::actual).has_value());
  REQUIRE(scalar_int(conn, "select count(*) from run_touches") == 2);
}

TEST_CASE("runs.lifecycle: touches filters by kind and orders by id", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());

  // Insert with paths in DESCENDING lexical order so an accidental
  // `order by path` would produce a different sequence than `order by id`.
  REQUIRE(rl::touch(conn, started->id, 2, "z.zig", rl::touch_kind::actual).has_value());
  REQUIRE(rl::touch(conn, started->id, 1, "a.zig", rl::touch_kind::declared).has_value());
  REQUIRE(rl::touch(conn, started->id, 3, "m.zig", rl::touch_kind::actual).has_value());

  auto all = rl::touches(conn, started->id, std::nullopt);
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 3);
  REQUIRE((*all)[0].path == "z.zig");
  REQUIRE((*all)[1].path == "a.zig");
  REQUIRE((*all)[2].path == "m.zig");

  auto actual_only = rl::touches(conn, started->id, rl::touch_kind::actual);
  REQUIRE(actual_only.has_value());
  REQUIRE(actual_only->size() == 2);
  REQUIRE((*actual_only)[0].path == "z.zig");
  REQUIRE((*actual_only)[1].path == "m.zig");

  auto declared_only = rl::touches(conn, started->id, rl::touch_kind::declared);
  REQUIRE(declared_only.has_value());
  REQUIRE(declared_only->size() == 1);
  REQUIRE((*declared_only)[0].path == "a.zig");
}

TEST_CASE("runs.lifecycle: finish is repeatable and the last call wins", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());
  REQUIRE(is_null(conn, "select ended_at from runs where id = 1"));

  REQUIRE(rl::finish(conn, started->id, "completed").has_value());
  REQUIRE(scalar_text(conn, "select status from runs where id = 1") == "completed");
  REQUIRE_FALSE(is_null(conn, "select ended_at from runs where id = 1"));

  // HAZARD 3: a second finish is NOT refused. Oracle-confirmed exit 0, and
  // the second status replaces the first.
  REQUIRE(rl::finish(conn, started->id, "aborted").has_value());
  REQUIRE(scalar_text(conn, "select status from runs where id = 1") == "aborted");
}

TEST_CASE("runs.lifecycle: finish on a missing id reports not_found, not success", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);

  auto missing = rl::finish(conn, 999, "completed");
  REQUIRE_FALSE(missing.has_value());
  // A bare UPDATE on a missing id affects zero rows and reports success; the
  // existence check is what turns it into the oracle's exit-1 "not found".
  REQUIRE(missing.error() == rl::runs_error::not_found);
}

TEST_CASE("runs.lifecycle: events and touches are accepted AFTER finish", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started =
      rl::start(conn, rl::start_args{.run_uid = "r1", .plan_id = 1, .arm = "strict", .base_sha = "s", .config_hash = "c"});
  REQUIRE(started.has_value());
  REQUIRE(rl::finish(conn, started->id, "completed").has_value());

  // HAZARD 3: the lifecycle is deliberately UNGUARDED. Both of these exit 0
  // against the oracle on a finished run. Pinning it so a future "helpful"
  // guard is a visible contract change rather than a silent one.
  REQUIRE(rl::event(conn, started->id, 20, "postfinish", std::nullopt).has_value());
  REQUIRE(rl::touch(conn, started->id, 1, "src/late.zig", rl::touch_kind::actual).has_value());
  REQUIRE(scalar_text(conn, "select status from runs where id = 1") == "completed");
}

TEST_CASE("runs.lifecycle: show and show_by_uid agree, and both report not_found", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = "r1",
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "deadbeef",
                                     .config_hash = "ch1",
                                     .corpus_repo = std::string_view{"/x"},
                                 });
  REQUIRE(started.has_value());

  auto by_id  = rl::show(conn, started->id);
  auto by_uid = rl::show_by_uid(conn, "r1");
  REQUIRE(by_id.has_value());
  REQUIRE(by_uid.has_value());
  REQUIRE(by_id->id == by_uid->id);
  REQUIRE(by_uid->run_uid == "r1");
  REQUIRE(by_uid->arm == "strict");
  REQUIRE(by_uid->base_sha == "deadbeef");
  REQUIRE(by_uid->config_hash == "ch1");
  REQUIRE(by_uid->corpus_repo == std::optional<std::string>{"/x"});
  REQUIRE_FALSE(by_uid->config_json.has_value());
  REQUIRE_FALSE(by_uid->ended_at.has_value());

  auto missing_id = rl::show(conn, 999);
  REQUIRE_FALSE(missing_id.has_value());
  REQUIRE(missing_id.error() == rl::runs_error::not_found);

  auto missing_uid = rl::show_by_uid(conn, "nope");
  REQUIRE_FALSE(missing_uid.has_value());
  REQUIRE(missing_uid.error() == rl::runs_error::not_found);
}

TEST_CASE("runs.lifecycle: events and touches on an unknown run are empty, not an error", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // `run show` / `bench show` gate on show_by_uid first, so these readers are
  // only ever reached with a real id -- but they must not invent an error for
  // a run with no children either, which is the empty-DB shape the oracle
  // renders as `events (0):`.
  auto evs = rl::events(conn, 999);
  REQUIRE(evs.has_value());
  REQUIRE(evs->empty());

  auto tch = rl::touches(conn, 999, std::nullopt);
  REQUIRE(tch.has_value());
  REQUIRE(tch->empty());
}

TEST_CASE("runs.lifecycle: generate_run_uid yields 32 lowercase hex characters", "[runs]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto uid = rl::generate_run_uid(conn);
  REQUIRE(uid.has_value());
  // Oracle-captured shape: "c5678087d1831bc7fe1e47a35d35f5f3" --
  // lower(hex(randomblob(16))).
  REQUIRE(uid->size() == 32);
  REQUIRE(std::ranges::all_of(*uid, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));

  auto second = rl::generate_run_uid(conn);
  REQUIRE(second.has_value());
  REQUIRE(*second != *uid);
}
