// @file session.t.cpp
// @brief Unit tests for `planar.engine.runtime.session` (plan 996, task
// 6094).
//
// The DB-visible expectations here were cross-checked against the oracle
// transcript captured in capture.t.cpp's header (the same probe session
// dumped `sessions` and `session_entries` with sqlite3 afterwards). In
// particular:
//
//   * two `planar capture session` runs with the same vendor tuple both
//     reported `"id":1` -- the row is idempotent on the tuple;
//   * that same pair left TWO `1|1|action|session opened` /
//     `1|2|action|session opened` entries -- the timeline append is NOT
//     idempotent;
//   * `session_entries.ordinal` came back dense and 1-based (1..9);
//   * `capture end` after ending the only `cli`/NULL session reported
//     `no active session` even though a live `claude`/`abc` session
//     existed -- an unset vendor_session_id matches NULL rows ONLY.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.session;

namespace {

namespace sess = planar::engine::runtime::session;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_session_test_{}_{}.db",
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

auto insert_task(planar::db::connection& conn, std::string_view title) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, title, status, priority) values ('global', '{}', 'todo', 100)", title));
  return scalar_int(conn, std::format("select id from tasks where title = '{}'", title));
}

/// RAII setenv/unsetenv guard -- Catch2 runs every TEST_CASE in one
/// process, so an env mutation that leaked would silently steer later
/// cases (and the `vendor defaults to cli` case in particular).
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

TEST_CASE("start_session inserts a row and reads it back", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto s = sess::start_session(conn, sess::start_args{.vendor = "claude", .vendor_session_id = "abc"});
  REQUIRE(s.has_value());
  CHECK(s->id > 0);
  CHECK(s->vendor == "claude");
  REQUIRE(s->vendor_session_id.has_value());
  CHECK(*s->vendor_session_id == "abc");
  CHECK_FALSE(s->ended_at.has_value());
  CHECK_FALSE(s->task_id.has_value());
  CHECK_FALSE(s->started_at.empty());
}

TEST_CASE("start_session is idempotent on the (vendor, vendor_session_id) tuple", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto first  = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  auto second = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(first->id == second->id);
  CHECK(scalar_int(conn, "select count(*) from sessions") == 1);
}

TEST_CASE("a differing vendor_session_id yields a distinct session", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto a = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  auto b = sess::start_session(conn, sess::start_args{.vendor = "cli", .vendor_session_id = "x"});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->id != b->id);
  CHECK(scalar_int(conn, "select count(*) from sessions") == 2);
}

TEST_CASE("an unset vendor_session_id matches only NULL rows, never any row", "[session]") {
  // This is the mechanism behind the oracle's `no active session` after
  // the cli/NULL session ended while a live claude/abc session remained.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(sess::start_session(conn, sess::start_args{.vendor = "cli", .vendor_session_id = "x"}).has_value());

  auto found = sess::active_for_vendor(conn, "cli", std::nullopt);
  REQUIRE(found.has_value());
  CHECK_FALSE(found->has_value());
}

TEST_CASE("reusing a session binds a previously unbound task", "[session]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  const auto      task_id = insert_task(conn, "T");

  auto first = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(first.has_value());
  CHECK_FALSE(first->task_id.has_value());

  auto second = sess::start_session(conn, sess::start_args{.vendor = "cli", .task_id = task_id});
  REQUIRE(second.has_value());
  CHECK(second->id == first->id);
  REQUIRE(second->task_id.has_value());
  CHECK(*second->task_id == task_id);
}

TEST_CASE("rebinding a session to a different task is refused", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      t1   = insert_task(conn, "T1");
  const auto      t2   = insert_task(conn, "T2");

  REQUIRE(sess::start_session(conn, sess::start_args{.vendor = "cli", .task_id = t1}).has_value());
  auto conflict = sess::start_session(conn, sess::start_args{.vendor = "cli", .task_id = t2});
  REQUIRE_FALSE(conflict.has_value());
  CHECK(conflict.error() == sess::session_error::task_conflict);
}

TEST_CASE("rebinding to the SAME task is accepted, not a conflict", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      t1   = insert_task(conn, "T1");

  REQUIRE(sess::start_session(conn, sess::start_args{.vendor = "cli", .task_id = t1}).has_value());
  auto again = sess::start_session(conn, sess::start_args{.vendor = "cli", .task_id = t1});
  REQUIRE(again.has_value());
  REQUIRE(again->task_id.has_value());
  CHECK(*again->task_id == t1);
}

TEST_CASE("model is written on INSERT only, never on reuse", "[session]") {
  // Preserves the Zig original's asymmetry: the reuse branch updates
  // task_id and nothing else.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  REQUIRE(sess::start_session(conn, sess::start_args{.vendor = "cli", .model = "m1"}).has_value());
  auto reused = sess::start_session(conn, sess::start_args{.vendor = "cli", .model = "m2"});
  REQUIRE(reused.has_value());
  REQUIRE(reused->model.has_value());
  CHECK(*reused->model == "m1");
}

TEST_CASE("end_session stamps ended_at and the summary", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  REQUIRE(sess::end_session(conn, s->id, "wrapped").has_value());
  auto after = sess::get_by_id(conn, s->id);
  REQUIRE(after.has_value());
  REQUIRE(after->ended_at.has_value());
  CHECK_FALSE(after->ended_at->empty());
  REQUIRE(after->summary.has_value());
  CHECK(*after->summary == "wrapped");
}

TEST_CASE("end_session without a summary leaves summary NULL", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  REQUIRE(sess::end_session(conn, s->id, std::nullopt).has_value());
  auto after = sess::get_by_id(conn, s->id);
  REQUIRE(after.has_value());
  REQUIRE(after->ended_at.has_value());
  CHECK_FALSE(after->summary.has_value());
}

TEST_CASE("ending an already-ended session is refused", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());
  REQUIRE(sess::end_session(conn, s->id, std::nullopt).has_value());

  auto again = sess::end_session(conn, s->id, std::nullopt);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error() == sess::session_error::already_ended);
}

TEST_CASE("ending a nonexistent session is not_found", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = sess::end_session(conn, 999, std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == sess::session_error::not_found);
}

TEST_CASE("active_for_vendor skips ended sessions", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  auto before = sess::active_for_vendor(conn, "cli", std::nullopt);
  REQUIRE(before.has_value());
  REQUIRE(before->has_value());

  REQUIRE(sess::end_session(conn, s->id, std::nullopt).has_value());
  auto after = sess::active_for_vendor(conn, "cli", std::nullopt);
  REQUIRE(after.has_value());
  CHECK_FALSE(after->has_value());
}

TEST_CASE("ensure_active creates a session when none is active", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  CHECK(scalar_int(conn, "select count(*) from sessions") == 0);

  auto id = sess::ensure_active(conn, "cli", std::nullopt);
  REQUIRE(id.has_value());
  CHECK(scalar_int(conn, "select count(*) from sessions") == 1);

  auto again = sess::ensure_active(conn, "cli", std::nullopt);
  REQUIRE(again.has_value());
  CHECK(*again == *id);
  CHECK(scalar_int(conn, "select count(*) from sessions") == 1);
}

TEST_CASE("ensure_active resurrects an ENDED session rather than opening a new one", "[session][parity]") {
  // SURPRISING, AND THE ORACLE'S OWN BEHAVIOR -- verified by running it,
  // after this test initially failed while asserting the intuitive
  // opposite:
  //
  //   $Z capture session --json     -> {"ok":true,"id":1,"vendor":"cli"}
  //   $Z capture end --json         -> {"ok":true,"id":1}
  //   $Z capture note "after end"   -> {"ok":true,"session_id":1}   [exit 0]
  //   sqlite3 ... 'select id,ended_at from sessions'  -> 1|2026-...Z
  //   sqlite3 ... session_entries -> 1|3|note|after end
  //
  // i.e. the note landed on the ALREADY-ENDED session 1; no second
  // session row was created. The mechanism is that `active_for_vendor`
  // filters on `ended_at is null` but `start_session`'s reuse lookup does
  // NOT -- so `ensure_active` misses the ended row, falls through to
  // `start_session`, and `start_session` finds and returns that very row.
  // The `sessions` table therefore accumulates at most one row per
  // (vendor, vendor_session_id) tuple forever, and `ended_at` is not a
  // barrier to further appends.
  //
  // Pinned as observed. Whether it is intended is a question for the
  // operator, not something to "fix" inside a behavior-preserving port.
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto            first = sess::ensure_active(conn, "cli", std::nullopt);
  REQUIRE(first.has_value());
  REQUIRE(sess::end_session(conn, *first, std::nullopt).has_value());

  auto second = sess::ensure_active(conn, "cli", std::nullopt);
  REQUIRE(second.has_value());
  CHECK(*second == *first);
  CHECK(scalar_int(conn, "select count(*) from sessions") == 1);
  // And the row really is still ended -- ensure_active does not reopen it.
  auto row = sess::get_by_id(conn, *second);
  REQUIRE(row.has_value());
  CHECK(row->ended_at.has_value());
}

TEST_CASE("append_entry numbers ordinals densely from 1", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());

  REQUIRE(sess::append_entry(conn, s->id, "action", "one").has_value());
  REQUIRE(sess::append_entry(conn, s->id, "note", "two").has_value());
  REQUIRE(sess::append_entry(conn, s->id, "command", "three").has_value());

  auto entries = sess::list_entries_for_session(conn, s->id);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 3);
  CHECK((*entries)[0].ordinal == 1);
  CHECK((*entries)[1].ordinal == 2);
  CHECK((*entries)[2].ordinal == 3);
  CHECK((*entries)[0].prefix == "action");
  CHECK((*entries)[1].prefix == "note");
  CHECK((*entries)[2].body == "three");
}

TEST_CASE("ordinals are per-session, not global", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            a    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  auto            b    = sess::start_session(conn, sess::start_args{.vendor = "claude", .vendor_session_id = "z"});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  REQUIRE(sess::append_entry(conn, a->id, "note", "a1").has_value());
  REQUIRE(sess::append_entry(conn, a->id, "note", "a2").has_value());
  REQUIRE(sess::append_entry(conn, b->id, "note", "b1").has_value());

  auto b_entries = sess::list_entries_for_session(conn, b->id);
  REQUIRE(b_entries.has_value());
  REQUIRE(b_entries->size() == 1);
  CHECK((*b_entries)[0].ordinal == 1);
}

TEST_CASE("appending to a nonexistent session fails on the foreign key", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = sess::append_entry(conn, 999, "note", "orphan");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == sess::session_error::query_failed);
}

TEST_CASE("set_start_git_context_if_unset writes once and never overwrites", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto            s    = sess::start_session(conn, sess::start_args{.vendor = "cli"});
  REQUIRE(s.has_value());
  CHECK_FALSE(s->repo_root.has_value());

  REQUIRE(sess::set_start_git_context_if_unset(conn, s->id, "/repo/one", "aaa").has_value());
  auto first = sess::get_by_id(conn, s->id);
  REQUIRE(first.has_value());
  REQUIRE(first->repo_root.has_value());
  CHECK(*first->repo_root == "/repo/one");
  REQUIRE(first->head_sha_at_start.has_value());
  CHECK(*first->head_sha_at_start == "aaa");

  // Second stamp with different values must be inert -- this is what
  // keeps a REUSED session's start boundary intact.
  REQUIRE(sess::set_start_git_context_if_unset(conn, s->id, "/repo/two", "bbb").has_value());
  auto second = sess::get_by_id(conn, s->id);
  REQUIRE(second.has_value());
  CHECK(*second->repo_root == "/repo/one");
  CHECK(*second->head_sha_at_start == "aaa");
}

TEST_CASE("get_by_id reports not_found for a missing session", "[session]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = sess::get_by_id(conn, 999);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == sess::session_error::not_found);
}

TEST_CASE("vendor_from_env defaults to cli when unset or empty", "[session][env]") {
  // Oracle: with $PLANAR_VENDOR unset, `capture session --json` reported
  // `"vendor":"cli"`.
  {
    env_guard g{"PLANAR_VENDOR", std::nullopt};
    CHECK(sess::vendor_from_env() == "cli");
  }
  {
    // An EMPTY value must also fall back -- the Zig guard is
    // `if (v.len > 0)`, not merely a presence check.
    env_guard g{"PLANAR_VENDOR", std::string_view{""}};
    CHECK(sess::vendor_from_env() == "cli");
  }
  {
    env_guard g{"PLANAR_VENDOR", std::string_view{"claude"}};
    CHECK(sess::vendor_from_env() == "claude");
  }
}

TEST_CASE("vendor_session_id_from_env treats empty as absent", "[session][env]") {
  {
    env_guard g{"PLANAR_VENDOR_SESSION_ID", std::nullopt};
    CHECK_FALSE(sess::vendor_session_id_from_env().has_value());
  }
  {
    env_guard g{"PLANAR_VENDOR_SESSION_ID", std::string_view{""}};
    CHECK_FALSE(sess::vendor_session_id_from_env().has_value());
  }
  {
    env_guard g{"PLANAR_VENDOR_SESSION_ID", std::string_view{"abc"}};
    auto      v = sess::vendor_session_id_from_env();
    REQUIRE(v.has_value());
    CHECK(*v == "abc");
  }
}
