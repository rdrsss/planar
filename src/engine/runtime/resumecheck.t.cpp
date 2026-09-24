// @file resumecheck.t.cpp
// @brief Unit tests for `planar.engine.runtime.resumecheck` (plan 996,
// task 6040).
//
// ORACLE PROVENANCE. Every rule below was DERIVED BY RUNNING the Zig
// binary against deliberately broken states in a pinned scratch arena —
// never from `--help`, and deliberately not by reading the validator
// first. The arena (`cd` FIRST, then `env`, because `VAR=x cd dir && bin`
// does not export past the `&&` on this platform's /bin/sh):
//
//   W=/tmp/oa; mkdir -p $W/{home,proj,fakehome}
//   cd $W/proj && env PLANAR_DB=$W/planar.db PLANAR_HOME=$W/home \
//     PLANAR_CONFIG_PATH=$W/config.toml HOME=$W/fakehome \
//     ./zig/zig-out/bin/planar <args>
//
//   $Z init ; $Z assoc create project:proj --kind project
//   $Z assoc add project:proj $W/proj ; $Z plan create "Demo plan"
//   $Z task add "Task no next-action"   --plan 1
//   $Z task add "Task with next-action" --plan 1 --next-action "do the thing"
//   $Z task add "Whitespace na"         --plan 1 --next-action " "
//
//   $Z resume validate 1 --json     -> exit 1
//     {"task_id":1,"resumable":false,"failures":[
//       {"check":"next_action","message":"next_action is null",
//        "remediation":"planar task update 1 --next-action \"<text>\""},
//       {"check":"snapshot","message":"no context snapshot found",
//        "remediation":"planar capture snapshot --task 1"}]}
//     stderr: error: task 1 is not resumable
//   $Z resume validate 2 --json     -> exit 1, snapshot failure ONLY
//   $Z resume validate 999 --json   -> exit 1, ZERO stdout,
//                                      stderr: error: task 999 not found
//   $Z resume validate 1            -> exit 1
//     FAIL task:1 is not resumable:
//       - next_action is null → run: planar task update 1 --next-action "<text>"
//       - no context snapshot found → run: planar capture snapshot --task 1
//
// THE FOUR NEGATIVE RESULTS — each one a probe that could have gone the
// other way, and each one pinned below:
//
//   1. STATUS IS NOT CHECKED.
//      $Z task update 2 --status doing ; $Z task update 2 --status done
//      $Z resume validate 2 --json -> {"task_id":2,"resumable":true,"failures":null}
//      A DONE task still validates.
//
//   2. THE next_action CHECK IS BYTE-LENGTH, NOT WHITESPACE-AWARE.
//      Task 3 was created with `--next-action " "` (a single space).
//      $Z resume validate 3 --json -> failures contains ONLY the snapshot
//      entry. The space PASSES. Trimming here would make a task the oracle
//      calls resumable non-resumable.
//
//   3. A SESSION-LEVEL SNAPSHOT DOES NOT COUNT.
//      $Z capture snapshot --note "sessionwide" --json
//          -> {"ok":true,"id":3,"session_id":1,"vendor":"cli"}   [no task_id]
//      $Z resume validate 3 --json -> STILL "no context snapshot found".
//      The lookup is `where task_id = ?`; a NULL task_id matches nothing.
//
//   4. AN EMPTY SNAPSHOT BODY IS ENOUGH.
//      $Z capture snapshot --task 1 --json   [no --note at all]
//      $Z resume validate 1 --json -> resumable:true once task 1 also had
//      a next_action. The check counts ROWS, not content.
//
// And the shape detail that bites callers: a resumable result serializes
// `"failures":null`, NOT `"failures":[]`. The `handoff` composite emits
// `[]` for the same data. Both are oracle-captured; they are different
// renderers on purpose.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.resumecheck;
import planar.engine.runtime.snapshot;

namespace {

namespace rck  = planar::engine::runtime::resumecheck;
namespace snap = planar::engine::runtime::snapshot;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_resumecheck_test_{}_{}.db",
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

/// @brief Insert a task with an explicit `next_action`.
auto insert_task(planar::db::connection& conn, std::string_view title, std::string_view next_action) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, title, status, priority, next_action) "
                         "values ('global', '{}', 'todo', 100, '{}')",
                         title, next_action));
  return scalar_int(conn, std::format("select id from tasks where title = '{}'", title));
}

/// @brief Insert a task whose `next_action` column is SQL NULL — the state
/// the oracle reports as `next_action is null`.
auto insert_task_null_action(planar::db::connection& conn, std::string_view title) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, title, status, priority) "
                         "values ('global', '{}', 'todo', 100)",
                         title));
  return scalar_int(conn, std::format("select id from tasks where title = '{}'", title));
}

auto new_session(planar::db::connection& conn) -> std::int64_t {
  exec(conn, "insert into sessions (vendor) values ('cli')");
  return scalar_int(conn, "select max(id) from sessions");
}

/// @brief Find the failure with `check == key`, or unset.
auto find_failure(const rck::validation_result& result, std::string_view key) -> std::optional<rck::validation_failure> {
  for (auto const& failure : result.failures) {
    if (failure.check == key) {
      return failure;
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("resumecheck validate: absent task is not_found, not a failure list", "[engine_runtime][resumecheck]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const checked = rck::validate(conn, 999);
  REQUIRE_FALSE(checked.has_value());
  // Distinct from "present but not resumable": the handler writes ZERO
  // bytes of payload on this path.
  REQUIRE(checked.error() == rck::resume_error::not_found);
}

TEST_CASE("resumecheck validate: both checks fail, in a fixed order", "[engine_runtime][resumecheck]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      task = insert_task_null_action(conn, "Task no next-action");

  auto const checked = rck::validate(conn, task);
  REQUIRE(checked.has_value());
  REQUIRE_FALSE(checked->resumable);
  REQUIRE(checked->task_id == task);
  REQUIRE(checked->failures.size() == 2);

  // The ORDER is part of the contract: the JSON array is ordered and a
  // caller may read failures[0].
  REQUIRE(checked->failures[0].check == "next_action");
  REQUIRE(checked->failures[1].check == "snapshot");

  REQUIRE(checked->failures[0].message == "next_action is null");
  REQUIRE(checked->failures[0].remediation == std::format("planar task update {} --next-action \"<text>\"", task));
  REQUIRE(checked->failures[1].message == "no context snapshot found");
  REQUIRE(checked->failures[1].remediation == std::format("planar capture snapshot --task {}", task));
}

TEST_CASE("resumecheck validate: a next_action of a single SPACE passes", "[engine_runtime][resumecheck]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      task = insert_task(conn, "Whitespace na", " ");

  auto const checked = rck::validate(conn, task);
  REQUIRE(checked.has_value());
  // Negative result #2 from the oracle probe: the check is byte-length,
  // not whitespace-aware. Only the snapshot check fires.
  REQUIRE_FALSE(find_failure(*checked, "next_action").has_value());
  REQUIRE(find_failure(*checked, "snapshot").has_value());
  REQUIRE(checked->failures.size() == 1);
}

TEST_CASE("resumecheck validate: a SESSION-level snapshot does not satisfy the check", "[engine_runtime][resumecheck]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      task    = insert_task(conn, "Has action", "do the thing");
  auto const      session = new_session(conn);

  // A snapshot with task_id NULL — exactly what `capture snapshot` with no
  // `--task` writes.
  auto const untargeted = snap::create(conn, snap::create_args{
                                                 .session_id        = session,
                                                 .task_id           = std::nullopt,
                                                 .vendor            = "cli",
                                                 .vendor_session_id = std::nullopt,
                                                 .body              = "sessionwide",
                                                 .next_action       = std::nullopt,
                                             });
  REQUIRE(untargeted.has_value());

  auto const checked = rck::validate(conn, task);
  REQUIRE(checked.has_value());
  // Negative result #3: still not resumable. `where task_id = ?` never
  // matches a NULL.
  REQUIRE_FALSE(checked->resumable);
  REQUIRE(find_failure(*checked, "snapshot").has_value());

  // Binding the SAME snapshot to the task flips it — which proves the
  // refusal was about the task binding and not about the row's existence.
  exec(conn, std::format("update context_snapshots set task_id = {} where id = {}", task, untargeted->id));
  auto const rechecked = rck::validate(conn, task);
  REQUIRE(rechecked.has_value());
  REQUIRE(rechecked->resumable);
}

TEST_CASE("resumecheck validate: an EMPTY snapshot body still satisfies the check", "[engine_runtime][resumecheck]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      task    = insert_task(conn, "Has action", "do the thing");
  auto const      session = new_session(conn);

  // No body at all — `capture snapshot --task N` with no --note.
  auto const empty = snap::create(conn, snap::create_args{
                                            .session_id        = session,
                                            .task_id           = task,
                                            .vendor            = "cli",
                                            .vendor_session_id = std::nullopt,
                                            .body              = std::nullopt,
                                            .next_action       = std::nullopt,
                                        });
  REQUIRE(empty.has_value());
  REQUIRE(empty->body.empty());

  auto const checked = rck::validate(conn, task);
  REQUIRE(checked.has_value());
  // Negative result #4: the check counts ROWS, not content.
  REQUIRE(checked->resumable);
  REQUIRE(checked->failures.empty());
}

TEST_CASE("resumecheck validate: task STATUS is not a check", "[engine_runtime][resumecheck]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      task    = insert_task(conn, "Has action", "do the thing");
  auto const      session = new_session(conn);
  REQUIRE(snap::create(conn, snap::create_args{.session_id        = session,
                                               .task_id           = task,
                                               .vendor            = "cli",
                                               .vendor_session_id = std::nullopt,
                                               .body              = "checkpoint body",
                                               .next_action       = std::nullopt})
              .has_value());

  // Negative result #1: drive the task all the way to a TERMINAL status
  // and it keeps validating. A status guard added here would be a
  // divergence, not a safety improvement.
  for (auto const status : {"doing", "done", "cancelled", "blocked"}) {
    exec(conn, std::format("update tasks set status = '{}' where id = {}", status, task));
    auto const checked = rck::validate(conn, task);
    REQUIRE(checked.has_value());
    REQUIRE(checked->resumable);
  }
}

TEST_CASE("resumecheck render_validate_json: resumable emits null, not an empty array", "[engine_runtime][resumecheck]") {
  rck::validation_result ok{.task_id = 2, .resumable = true, .failures = {}};
  // The exact oracle bytes, terminator included.
  REQUIRE(rck::render_validate_json(ok) == "{\"task_id\":2,\"resumable\":true,\"failures\":null}\n");
  // Specifically NOT the empty array the handoff composite emits.
  REQUIRE(rck::render_validate_json(ok).find("[]") == std::string::npos);
}

TEST_CASE("resumecheck render_validate_json: failures serialize in order with escaped text", "[engine_runtime][resumecheck]") {
  rck::validation_result bad{
      .task_id   = 1,
      .resumable = false,
      .failures  = {rck::validation_failure{.check       = "next_action",
                                            .message     = "next_action is null",
                                            .remediation = "planar task update 1 --next-action \"<text>\""},
                    rck::validation_failure{.check       = "snapshot",
                                            .message     = "no context snapshot found",
                                            .remediation = "planar capture snapshot --task 1"}},
  };
  REQUIRE(rck::render_validate_json(bad) == "{\"task_id\":1,\"resumable\":false,\"failures\":["
                                            "{\"check\":\"next_action\",\"message\":\"next_action is null\","
                                            "\"remediation\":\"planar task update 1 --next-action \\\"<text>\\\"\"},"
                                            "{\"check\":\"snapshot\",\"message\":\"no context snapshot found\","
                                            "\"remediation\":\"planar capture snapshot --task 1\"}]}\n");
}

TEST_CASE("resumecheck render_validate_text: both forms, byte-exact", "[engine_runtime][resumecheck]") {
  rck::validation_result ok{.task_id = 2, .resumable = true, .failures = {}};
  REQUIRE(rck::render_validate_text(ok) == "OK task:2 is resume-ready\n");

  rck::validation_result bad{
      .task_id   = 1,
      .resumable = false,
      .failures  = {rck::validation_failure{.check       = "next_action",
                                            .message     = "next_action is null",
                                            .remediation = "planar task update 1 --next-action \"<text>\""}},
  };
  // U+2192 between message and `run:`, two-space indent, `- ` bullet.
  REQUIRE(rck::render_validate_text(bad) == "FAIL task:1 is not resumable:\n"
                                            "  - next_action is null \xe2\x86\x92 run: "
                                            "planar task update 1 --next-action \"<text>\"\n");
}
