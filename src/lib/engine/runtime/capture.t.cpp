// @file capture.t.cpp
// @brief Unit tests for `planar.engine.runtime.capture` and
// `planar.engine.runtime.snapshot` (plan 996, task 6094).
//
// ORACLE PROVENANCE. Every rendered string and every timeline shape below
// was captured by RUNNING the Zig binary against a scratch database, never
// from `--help` and never from reading the Zig source:
//
//   cd /tmp/oracle/repo && git init -q .
//   export PLANAR_DB=/tmp/oracle/p9.db PLANAR_CONFIG_PATH=/tmp/oracle/p.toml
//   Z=./zig/zig-out/bin/planar ; $Z init
//
//   $Z capture session --json
//       -> {"ok":true,"id":1,"vendor":"cli"}
//   $Z capture session
//       -> session 1 opened (vendor: cli)            [SAME id -- reused]
//   $Z capture session --vendor claude --vendor-session-id abc --model m1 --json
//       -> {"ok":true,"id":2,"vendor":"claude","vendor_session_id":"abc"}
//          [note: no "model" key in the envelope at all]
//   $Z capture note "hello note" --json   -> {"ok":true,"session_id":1}
//   $Z capture note "hello note 2"        -> captured note in session 1
//   $Z capture command "ls -la" --outcome ok --json -> {"ok":true,"session_id":1}
//   $Z capture file "src/x.zig" --role edited --json -> {"ok":true,"session_id":1}
//   $Z capture snapshot "body here" --next-action "do it" --json
//       -> {"ok":true,"id":1,"session_id":1,"vendor":"cli"}
//   $Z capture snapshot "body 2"          -> snapshot 2 created (vendor: cli)
//   $Z capture end --summary "wrapped" --json -> {"ok":true,"id":1}
//   $Z capture end --json                 -> exit 1, error: no active session
//   $Z capture end 999 --json             -> exit 1, error: session 999 not found
//
//   sqlite3 p9.db 'select session_id,ordinal,prefix,body from session_entries'
//       1|1|action |session opened
//       1|2|action |session opened          <-- append is NOT idempotent
//       1|3|note   |hello note
//       1|4|note   |hello note 2
//       1|5|command|ls -la\noutcome: ok     <-- two lines
//       1|6|file   |src/x.zig [edited]      <-- " [role]" suffix
//       1|7|note   |snapshot created: id=1
//       1|8|note   |snapshot created: id=2
//       1|9|note   |session ended           <-- BEFORE ended_at is stamped
//       2|1|action |session opened
//
//   sqlite3 p9.db 'select id,session_id,task_id,vendor,vendor_session_id,body,next_action from context_snapshots'
//       1|1||cli||body here|do it
//       2|1||cli||body 2|                   <-- next_action stored NULL
//
// `capture commits` is NOT covered: it is not ported (see capture.cppm's
// cut list -- it rests on 1205 lines of git shelling). That is the one
// `capture` leaf this cycle does not reach.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.capture;
import planar.engine.runtime.session;
import planar.engine.runtime.snapshot;

namespace {

namespace cap  = planar::engine::runtime::capture;
namespace sess = planar::engine::runtime::session;
namespace snap = planar::engine::runtime::snapshot;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_capture_test_{}_{}.db",
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

auto is_null(planar::db::connection& conn, std::string_view sql) -> bool {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->is_null(0);
}

auto insert_task(planar::db::connection& conn, std::string_view title, std::string_view next_action) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, title, status, priority, next_action) "
                         "values ('global', '{}', 'todo', 100, '{}')",
                         title, next_action));
  return scalar_int(conn, std::format("select id from tasks where title = '{}'", title));
}

struct env_guard {
  std::string                name_;
  std::optional<std::string> saved_;

  env_guard(const char* name, std::optional<std::string_view> value) : name_(name) {
    if (const char* prev = std::getenv(name); prev != nullptr) {
      saved_ = std::string{prev};
    }
    if (value.has_value()) {
      ::setenv(name, std::string{*value}.c_str(), 1);
    } else {
      ::unsetenv(name);
    }
  }

  env_guard(const env_guard&)            = delete;
  env_guard& operator=(const env_guard&) = delete;

  ~env_guard() {
    if (saved_.has_value()) {
      ::setenv(name_.c_str(), saved_->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }
};

} // namespace

// ---------------------------------------------------------------------------
// capture session
// ---------------------------------------------------------------------------

TEST_CASE("open_session appends exactly one session-opened marker", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto s = cap::open_session(conn, cap::open_args{.vendor = "v"});
  REQUIRE(s.has_value());

  auto entries = sess::list_entries_for_session(conn, s->id);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
  CHECK((*entries)[0].prefix == "action");
  CHECK((*entries)[0].body == "session opened");
}

TEST_CASE("re-opening the same session reuses the row but appends a SECOND marker", "[capture][parity]") {
  // Pinned deliberately: the oracle's two `capture session` runs both
  // reported id 1 and left TWO `action|session opened` rows. A port that
  // suppressed the duplicate would diverge from the observed timeline.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto first  = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  auto second = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(first->id == second->id);

  auto entries = sess::list_entries_for_session(conn, first->id);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);
  CHECK((*entries)[0].body == "session opened");
  CHECK((*entries)[1].body == "session opened");
  CHECK((*entries)[1].ordinal == 2);
}

TEST_CASE("open_session stamps the supplied git context onto the row", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto s = cap::open_session(conn, cap::open_args{.vendor = "cli"},
                             cap::start_git_context{.repo_root = "/repo", .head_sha_at_start = "deadbeef"});
  REQUIRE(s.has_value());
  REQUIRE(s->repo_root.has_value());
  CHECK(*s->repo_root == "/repo");
  REQUIRE(s->head_sha_at_start.has_value());
  CHECK(*s->head_sha_at_start == "deadbeef");
}

TEST_CASE("a reused session keeps its ORIGINAL git start boundary", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  REQUIRE(cap::open_session(conn, cap::open_args{.vendor = "cli"},
                            cap::start_git_context{.repo_root = "/first", .head_sha_at_start = "aaa"})
              .has_value());
  auto second = cap::open_session(conn, cap::open_args{.vendor = "cli"},
                                  cap::start_git_context{.repo_root = "/second", .head_sha_at_start = "bbb"});
  REQUIRE(second.has_value());
  CHECK(*second->repo_root == "/first");
  CHECK(*second->head_sha_at_start == "aaa");
}

TEST_CASE("omitting the git context leaves both columns NULL", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto s = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  REQUIRE(s.has_value());
  CHECK_FALSE(s->repo_root.has_value());
  CHECK_FALSE(s->head_sha_at_start.has_value());
}

TEST_CASE("a conflicting task rebind surfaces as task_conflict", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      t1   = insert_task(conn, "T1", "");
  const auto      t2   = insert_task(conn, "T2", "");

  REQUIRE(cap::open_session(conn, cap::open_args{.vendor = "cli", .task_id = t1}).has_value());
  auto res = cap::open_session(conn, cap::open_args{.vendor = "cli", .task_id = t2});
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == cap::capture_error::task_conflict);
}

TEST_CASE("capture session renderers match the oracle byte for byte", "[capture][parity]") {
  sess::session plain{.id = 1, .vendor = "cli"};
  CHECK(cap::render_session_json(plain) == R"({"ok":true,"id":1,"vendor":"cli"})");
  CHECK(cap::render_session_text(plain) == "session 1 opened (vendor: cli)");

  sess::session vendored{.id = 2, .vendor = "claude", .vendor_session_id = std::string{"abc"}};
  // The oracle emitted NO "model" key even though --model m1 was passed
  // and stored -- the envelope carries id/vendor/vendor_session_id/task_id
  // only.
  CHECK(cap::render_session_json(vendored) == R"({"ok":true,"id":2,"vendor":"claude","vendor_session_id":"abc"})");
  CHECK(cap::render_session_text(vendored) == "session 2 opened (vendor: claude, vsid: abc)");

  sess::session bound{.id = 3, .task_id = 42, .vendor = "cli", .vendor_session_id = std::string{"z"}};
  CHECK(cap::render_session_json(bound) == R"({"ok":true,"id":3,"vendor":"cli","vendor_session_id":"z","task_id":42})");
  CHECK(cap::render_session_text(bound) == "session 3 opened (vendor: cli, vsid: z, task: 42)");
}

// ---------------------------------------------------------------------------
// capture note / command / file
// ---------------------------------------------------------------------------

TEST_CASE("append_note, append_command and append_file use their own prefixes", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  REQUIRE(cap::append_note(conn, s->id, "n").has_value());
  REQUIRE(cap::append_command(conn, s->id, "c").has_value());
  REQUIRE(cap::append_file(conn, s->id, "f").has_value());

  auto entries = sess::list_entries_for_session(conn, s->id);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 3);
  CHECK((*entries)[0].prefix == "note");
  CHECK((*entries)[1].prefix == "command");
  CHECK((*entries)[2].prefix == "file");
}

TEST_CASE("compose_command_body puts the outcome on its own line", "[capture][parity]") {
  // Oracle stored `ls -la\noutcome: ok` as ONE entry body spanning two
  // lines -- not `ls -la (ok)` and not a separate entry.
  CHECK(cap::compose_command_body("ls -la", "ok") == "ls -la\noutcome: ok");
  CHECK(cap::compose_command_body("ls -la", std::nullopt) == "ls -la");
  // An EMPTY outcome is still an outcome: the Zig branch is on the
  // optional's presence, not on its length.
  CHECK(cap::compose_command_body("x", std::string_view{""}) == "x\noutcome: ");
}

TEST_CASE("compose_file_body appends a bracketed role", "[capture][parity]") {
  // Oracle stored `src/x.zig [edited]`.
  CHECK(cap::compose_file_body("src/x.zig", "edited") == "src/x.zig [edited]");
  CHECK(cap::compose_file_body("src/x.zig", std::nullopt) == "src/x.zig");
  CHECK(cap::compose_file_body("p", std::string_view{""}) == "p []");
}

TEST_CASE("the append envelopes match the oracle byte for byte", "[capture][parity]") {
  CHECK(cap::render_append_json(1) == R"({"ok":true,"session_id":1})");
  CHECK(cap::render_append_text("note", 1) == "captured note in session 1");
  CHECK(cap::render_append_text("command", 7) == "captured command in session 7");
  CHECK(cap::render_append_text("file", 7) == "captured file in session 7");
}

// ---------------------------------------------------------------------------
// session resolution
// ---------------------------------------------------------------------------

TEST_CASE("resolve_session_id honours an explicit --session without touching the DB", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto id = cap::resolve_session_id(conn, 4242);
  REQUIRE(id.has_value());
  CHECK(*id == 4242);
  // Nothing was created: the explicit branch short-circuits.
  CHECK(scalar_int(conn, "select count(*) from sessions") == 0);
}

TEST_CASE("resolve_session_id CREATES a session when none is active", "[capture]") {
  // This is why `planar capture note "x"` works with no prior
  // `planar capture session`.
  env_guard       v{"PLANAR_VENDOR", std::nullopt};
  env_guard       vs{"PLANAR_VENDOR_SESSION_ID", std::nullopt};
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto id = cap::resolve_session_id(conn, std::nullopt);
  REQUIRE(id.has_value());
  CHECK(scalar_int(conn, "select count(*) from sessions") == 1);
  CHECK(scalar_int(conn, std::format("select count(*) from sessions where id = {} and vendor = 'cli'", *id)) == 1);
}

TEST_CASE("resolve_session_id follows $PLANAR_VENDOR", "[capture][env]") {
  env_guard       v{"PLANAR_VENDOR", std::string_view{"codex"}};
  env_guard       vs{"PLANAR_VENDOR_SESSION_ID", std::string_view{"s-9"}};
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto id = cap::resolve_session_id(conn, std::nullopt);
  REQUIRE(id.has_value());
  CHECK(scalar_int(conn, std::format("select count(*) from sessions where id = {} and vendor = 'codex' "
                                     "and vendor_session_id = 's-9'",
                                     *id)) == 1);
}

TEST_CASE("resolve_existing_session_id NEVER creates a session", "[capture]") {
  // The `capture end` resolver. Oracle: `capture end --json` with no
  // active session exits 1 with `error: no active session` -- it does not
  // silently open one and end it.
  env_guard       v{"PLANAR_VENDOR", std::nullopt};
  env_guard       vs{"PLANAR_VENDOR_SESSION_ID", std::nullopt};
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = cap::resolve_existing_session_id(conn, std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == cap::capture_error::no_active_session);
  CHECK(scalar_int(conn, "select count(*) from sessions") == 0);
}

TEST_CASE("resolve_existing_session_id finds the active session", "[capture]") {
  env_guard       v{"PLANAR_VENDOR", std::nullopt};
  env_guard       vs{"PLANAR_VENDOR_SESSION_ID", std::nullopt};
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  auto res = cap::resolve_existing_session_id(conn, std::nullopt);
  REQUIRE(res.has_value());
  CHECK(*res == s->id);
}

// ---------------------------------------------------------------------------
// capture end
// ---------------------------------------------------------------------------

TEST_CASE("close_session appends the note BEFORE stamping ended_at", "[capture][parity]") {
  // Oracle timeline: `session ended` was ordinal 9, the LAST entry, and
  // the row's ended_at was set. Ordering matters -- the boundary note has
  // to land while the session is still open.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  REQUIRE(cap::close_session(conn, s->id, "wrapped").has_value());

  auto entries = sess::list_entries_for_session(conn, s->id);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 2);
  CHECK(entries->back().prefix == "note");
  CHECK(entries->back().body == "session ended");

  auto after = sess::get_by_id(conn, s->id);
  REQUIRE(after.has_value());
  REQUIRE(after->ended_at.has_value());
  REQUIRE(after->summary.has_value());
  CHECK(*after->summary == "wrapped");
}

TEST_CASE("closing an already-closed session is refused", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  REQUIRE(s.has_value());
  REQUIRE(cap::close_session(conn, s->id, std::nullopt).has_value());

  auto res = cap::close_session(conn, s->id, std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == cap::capture_error::already_ended);
}

TEST_CASE("closing a nonexistent session is not_found", "[capture]") {
  // Oracle: `capture end 999 --json` -> exit 1, `error: session 999 not found`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = cap::close_session(conn, 999, std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == cap::capture_error::not_found);
}

TEST_CASE("the capture end envelopes match the oracle byte for byte", "[capture][parity]") {
  CHECK(cap::render_end_json(1) == R"({"ok":true,"id":1})");
  CHECK(cap::render_end_text(1) == "session 1 ended");
}

// ---------------------------------------------------------------------------
// capture snapshot
// ---------------------------------------------------------------------------

TEST_CASE("take_snapshot stores the row and notes it on the timeline", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = cap::open_session(conn, cap::open_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  auto snapshot = cap::take_snapshot(
      conn, cap::snapshot_args{.session_id = s->id, .vendor = "cli", .body = "body here", .next_action = "do it"});
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->body == "body here");
  CHECK(snapshot->next_action == "do it");

  auto entries = sess::list_entries_for_session(conn, s->id);
  REQUIRE(entries.has_value());
  CHECK(entries->back().prefix == "note");
  CHECK(entries->back().body == std::format("snapshot created: id={}", snapshot->id));
}

TEST_CASE("an empty body or next_action is stored as SQL NULL, not ''", "[capture][snapshot]") {
  // Oracle: `capture snapshot "body 2"` (no --next-action, no bound task)
  // stored next_action NULL. The read path coalesces it back to "".
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  auto stored = snap::create(conn, snap::create_args{.session_id = s->id, .vendor = "cli", .body = "body 2"});
  REQUIRE(stored.has_value());
  CHECK(stored->next_action.empty());
  CHECK(is_null(conn, std::format("select next_action from context_snapshots where id = {}", stored->id)));
  CHECK_FALSE(is_null(conn, std::format("select body from context_snapshots where id = {}", stored->id)));

  auto both_empty = snap::create(
      conn,
      snap::create_args{.session_id = s->id, .vendor = "cli", .body = std::string_view{""}, .next_action = std::string_view{""}});
  REQUIRE(both_empty.has_value());
  CHECK(is_null(conn, std::format("select body from context_snapshots where id = {}", both_empty->id)));
  CHECK(both_empty->body.empty());
}

TEST_CASE("snapshot show reports not_found for a missing id", "[capture][snapshot]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = snap::show(conn, 999);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == snap::snapshot_error::not_found);
}

TEST_CASE("get_latest_for_task returns the newest snapshot, list_for_task returns all", "[capture][snapshot]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      task_id = insert_task(conn, "T", "");
  auto            s       = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  auto first  = snap::create(conn, snap::create_args{.session_id = s->id, .task_id = task_id, .vendor = "cli", .body = "one"});
  auto second = snap::create(conn, snap::create_args{.session_id = s->id, .task_id = task_id, .vendor = "cli", .body = "two"});
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  auto latest = snap::get_latest_for_task(conn, task_id);
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  // Ordering is (created_at DESC, id DESC): both rows land in the same
  // millisecond here, so the id tiebreak is what makes this deterministic.
  CHECK((*latest)->id == second->id);

  auto all = snap::list_for_task(conn, task_id);
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  CHECK((*all)[0].id == second->id);
  CHECK((*all)[1].id == first->id);
}

TEST_CASE("get_latest_for_task is empty for a task with no snapshots", "[capture][snapshot]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      task_id = insert_task(conn, "T", "");

  auto latest = snap::get_latest_for_task(conn, task_id);
  REQUIRE(latest.has_value());
  CHECK_FALSE(latest->has_value());
}

TEST_CASE("resolve_next_action prefers --next-action over the task column", "[capture]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      task_id = insert_task(conn, "T", "from task");

  auto explicit_value = cap::resolve_next_action(conn, "from flag", task_id);
  REQUIRE(explicit_value.has_value());
  REQUIRE(explicit_value->has_value());
  CHECK(**explicit_value == "from flag");
}

TEST_CASE("resolve_next_action falls back to the bound task's next_action", "[capture]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      task_id = insert_task(conn, "T", "from task");

  auto fallback = cap::resolve_next_action(conn, std::nullopt, task_id);
  REQUIRE(fallback.has_value());
  REQUIRE(fallback->has_value());
  CHECK(**fallback == "from task");
}

TEST_CASE("resolve_next_action treats an empty task next_action as absent", "[capture]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      task_id = insert_task(conn, "T", "");

  auto res = cap::resolve_next_action(conn, std::nullopt, task_id);
  REQUIRE(res.has_value());
  CHECK_FALSE(res->has_value());
}

TEST_CASE("resolve_next_action with no task and no flag is absent, not an error", "[capture]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto no_task = cap::resolve_next_action(conn, std::nullopt, std::nullopt);
  REQUIRE(no_task.has_value());
  CHECK_FALSE(no_task->has_value());

  // A task id that does not exist is ALSO not an error -- the Zig
  // handler's `.done` arm is an empty block.
  auto missing_task = cap::resolve_next_action(conn, std::nullopt, 4242);
  REQUIRE(missing_task.has_value());
  CHECK_FALSE(missing_task->has_value());
}

TEST_CASE("the capture snapshot envelopes match the oracle byte for byte", "[capture][parity]") {
  snap::snapshot plain{.id = 1, .session_id = 1, .vendor = "cli"};
  CHECK(cap::render_snapshot_json(plain) == R"({"ok":true,"id":1,"session_id":1,"vendor":"cli"})");
  CHECK(cap::render_snapshot_text(plain) == "snapshot 1 created (vendor: cli)");

  snap::snapshot bound{.id = 2, .session_id = 1, .task_id = 42, .vendor = "cli"};
  CHECK(cap::render_snapshot_json(bound) == R"({"ok":true,"id":2,"session_id":1,"vendor":"cli","task_id":42})");
  CHECK(cap::render_snapshot_text(bound) == "snapshot 2 created (vendor: cli, task: 42)");
}
