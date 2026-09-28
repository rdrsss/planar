// @file harvest.t.cpp
// @brief Unit tests for `planar.engine.runs.harvest` (plan 996, task 6362).
//
// ORACLE PROVENANCE. `zig/src/engine/runs/harvest.zig`'s OWN test block is
// the provenance for the shapes pinned here (working-tree picks up
// uncommitted edits AND untracked files, range diffs base..head, harvest
// writes `kind='actual'` rows, a clean worktree writes zero, harvest is
// idempotent) -- that file's tests build real throwaway git repos exactly
// as this one does, so there was no separate oracle-binary probe to run
// for behaviour git itself defines. This tree's own `git.t.cpp` established
// the same throwaway-repo pattern reused below.
//
// Every repo-building case skips itself when `git` is not on PATH, the same
// discipline `git.t.cpp` uses, so the suite stays runnable on a machine
// without it.

// Include-before-import is deliberate (see db/db.t.cpp): `::popen`/
// `::pclose` in `rev_parse_head` below need `<cstdio>` in the global module
// fragment.
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runs.lifecycle;
import planar.engine.runs.harvest;

namespace {

namespace rl = planar::engine::runs::lifecycle;
namespace rh = planar::engine::runs::harvest;

// ---------------------------------------------------------------------------
// Scratch database (same shape as lifecycle.t.cpp's).
// ---------------------------------------------------------------------------

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_harvest_test_{}_{}.db",
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

auto seed_bare_plan(planar::db::connection& conn) -> void {
  exec(conn, "insert into plans (scope_kind, title, slug, status) "
             "values ('global', 'Harvest Plan', 'harvest-plan', 'draft')");
}

auto seed_run(planar::db::connection& conn, std::string_view run_uid) -> std::int64_t {
  auto started = rl::start(conn, rl::start_args{
                                     .run_uid     = run_uid,
                                     .plan_id     = 1,
                                     .arm         = "strict",
                                     .base_sha    = "base",
                                     .config_hash = "h",
                                 });
  REQUIRE(started.has_value());
  return started->id;
}

auto touch_count(planar::db::connection& conn, std::int64_t run_id, std::string_view kind) -> std::int64_t {
  auto stmt = conn.prepare("select count(*) from run_touches where run_id = ? and kind = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, run_id).has_value());
  REQUIRE(stmt->bind_text(2, kind).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

// ---------------------------------------------------------------------------
// Real git fixtures (same pattern as lib/git/git.t.cpp).
// ---------------------------------------------------------------------------

struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_harvest_repo_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)                        = delete;
  auto operator=(const scratch_dir&) -> scratch_dir&     = delete;
  scratch_dir(scratch_dir&&) noexcept                    = delete;
  auto operator=(scratch_dir&&) noexcept -> scratch_dir& = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] auto get() const -> const std::filesystem::path& {
    return path_;
  }
};

auto have_git() -> bool {
  static bool const answer = std::system("git --version >/dev/null 2>&1") == 0;
  return answer;
}

/// @brief Run a shell line inside `dir`, aborting the test on failure.
/// FIXTURE construction only, never the behaviour under test.
auto fixture_sh(const std::filesystem::path& dir, std::string_view line) -> bool {
  std::string const composed = std::format("cd '{}' && {} >/dev/null 2>&1", dir.string(), line);
  return std::system(composed.c_str()) == 0;
}

auto init_repo(const std::filesystem::path& dir) -> bool {
  return fixture_sh(dir, "git init -q -b main") && fixture_sh(dir, "git config user.email planar@example.invalid") &&
         fixture_sh(dir, "git config user.name Planar");
}

auto write_file(const std::filesystem::path& dir, std::string_view rel, std::string_view content) -> void {
  std::ofstream out(dir / rel);
  out << content;
}

auto commit_all(const std::filesystem::path& dir, std::string_view message) -> bool {
  return fixture_sh(dir, "git add -A") && fixture_sh(dir, std::format("git commit -q -m '{}'", message));
}

auto rev_parse_head(const std::filesystem::path& dir) -> std::string {
  std::string const cmd  = std::format("cd '{}' && git rev-parse HEAD", dir.string());
  std::FILE*        pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string           out;
  std::array<char, 128> buf{};
  std::size_t           n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
    out.append(buf.data(), n);
  }
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_paths — the DISTINCT guarantee, pinned WITHOUT a subprocess.
// ---------------------------------------------------------------------------

TEST_CASE("harvest.parse_paths de-duplicates a path repeated within one blob, and drops blank lines", "[runs][harvest]") {
  // The task-6362 acceptance requirement, verbatim: a path appearing TWICE
  // in one diff must produce exactly ONE entry. `src/a.zig` here is that
  // repeat; `harvest()` writes one `run_touches` row per element of this
  // return value via `touch_idempotent`, so collapsing the duplicate HERE
  // is what makes the write count correct downstream.
  auto const paths = rh::parse_paths("src/a.zig\nsrc/b.zig\n\nsrc/a.zig\n  \nsrc/c.zig\n");
  REQUIRE(paths.size() == 3);
  CHECK(paths[0] == "src/a.zig");
  CHECK(paths[1] == "src/b.zig");
  CHECK(paths[2] == "src/c.zig");
}

TEST_CASE("harvest.parse_paths returns empty for blank or empty input", "[runs][harvest]") {
  CHECK(rh::parse_paths("").empty());
  CHECK(rh::parse_paths("\n\n  \n").empty());
}

// ---------------------------------------------------------------------------
// diff_paths / harvest — real git repositories.
// ---------------------------------------------------------------------------

TEST_CASE("harvest: working-tree mode picks up a modified tracked file AND an untracked new file", "[runs][harvest]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "tracked.txt", "v1\n");
  REQUIRE(commit_all(repo.get(), "base"));

  // Modify the tracked file (visible to `git diff --name-only HEAD`) and
  // add a brand-new file WITHOUT staging it (visible only to
  // `git ls-files --others --exclude-standard` -- the oracle's task-4289
  // fix this port reproduces).
  write_file(repo.get(), "tracked.txt", "v2\n");
  write_file(repo.get(), "untracked_new.txt", "brand new\n");

  auto const paths = rh::diff_paths(repo.get(), std::nullopt);
  REQUIRE(paths.has_value());
  REQUIRE(paths->size() == 2);
  CHECK(std::ranges::find(*paths, "tracked.txt") != paths->end());
  CHECK(std::ranges::find(*paths, "untracked_new.txt") != paths->end());
}

TEST_CASE("harvest: range mode diffs base..head across commits, no ls-files pass", "[runs][harvest]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "a.txt", "a\n");
  REQUIRE(commit_all(repo.get(), "base"));
  auto const base = rev_parse_head(repo.get());

  write_file(repo.get(), "b.txt", "b\n");
  REQUIRE(commit_all(repo.get(), "c1"));
  write_file(repo.get(), "c.txt", "c\n");
  REQUIRE(commit_all(repo.get(), "c2"));
  auto const head = rev_parse_head(repo.get());

  auto const paths = rh::diff_paths(repo.get(), rh::diff_range{.base = base, .head = head});
  REQUIRE(paths.has_value());
  REQUIRE(paths->size() == 2);
  CHECK(std::ranges::find(*paths, "b.txt") != paths->end());
  CHECK(std::ranges::find(*paths, "c.txt") != paths->end());
  // Committed new files are visible through `diff` alone. This fixture
  // alone does NOT prove the `ls-files --others` pass is skipped in range
  // mode -- a clean worktree can't distinguish "the pass ran and found
  // nothing" from "the pass never ran" (see the next test, which is the
  // one that actually discriminates).
}

TEST_CASE("harvest: range mode does NOT run the untracked-file pass, even with one dangling in the worktree", "[runs][harvest]") {
  // The gap a same-clean-worktree fixture cannot see (review round 2 on
  // task 6362): every OTHER range-mode fixture in this file leaves a
  // clean working tree after its commits, so `git ls-files --others`
  // would return nothing whether or not it were called -- the mutation
  // `if (!spec.has_value())` -> `if (true)` at harvest.cpp:92 SURVIVED
  // against all of them. This fixture leaves an untracked file DANGLING
  // in the worktree at harvest time, which is exactly the coder-worktree
  // scenario the module header's load-bearing claim
  // ("no `ls-files` pass is needed or run" for range mode) is about: a
  // regression that ran the untracked pass in range mode would pull
  // `scratch.txt` into the result, polluting the harvested touch set with
  // a path never in the committed range.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "a.txt", "a\n");
  REQUIRE(commit_all(repo.get(), "base"));
  auto const base = rev_parse_head(repo.get());

  write_file(repo.get(), "b.txt", "b\n");
  REQUIRE(commit_all(repo.get(), "committed"));
  auto const head = rev_parse_head(repo.get());

  // Dangling, untracked, unrelated to the committed range -- never staged,
  // never committed. Present in the worktree at the moment `diff_paths` is
  // called.
  write_file(repo.get(), "scratch.txt", "wip\n");

  auto const paths = rh::diff_paths(repo.get(), rh::diff_range{.base = base, .head = head});
  REQUIRE(paths.has_value());
  REQUIRE(paths->size() == 1);
  CHECK(paths->front() == "b.txt");
  CHECK(std::ranges::find(*paths, "scratch.txt") == paths->end());
}

TEST_CASE("harvest: range mode against a nonexistent worktree reports git_failed via the single query", "[runs][harvest]") {
  // The other half of the same review finding: every EXISTING git_failed
  // fixture in this file uses working-tree mode, where the untracked-file
  // pass's OWN `!untracked_raw.has_value()` guard independently produces
  // `git_failed` -- so a mutation that neutered the FIRST query's guard
  // (`!raw.has_value()` at harvest.cpp:82) would survive every fixture in
  // this file, because range mode never ran and working-tree mode's
  // second guard masked the first. Range mode issues exactly ONE query,
  // so this is the only path that isolates that guard.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  auto const paths =
      rh::diff_paths("/nonexistent/planar-harvest/range/xyzzy", rh::diff_range{.base = "deadbeef", .head = "cafef00d"});
  REQUIRE_FALSE(paths.has_value());
  CHECK(paths.error() == rh::harvest_error::git_failed);
}

TEST_CASE("harvest: writes one kind='actual' row per distinct path and returns the count", "[runs][harvest]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "x.txt", "1\n");
  REQUIRE(commit_all(repo.get(), "base"));
  write_file(repo.get(), "x.txt", "2\n");
  write_file(repo.get(), "y.txt", "new\n");

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto const run_id = seed_run(conn, "uid-harvest");

  auto const n = rh::harvest(conn, rh::harvest_args{.worktree = repo.get(), .run_id = run_id, .task_id = 42});
  REQUIRE(n.has_value());
  CHECK(*n == 2);
  CHECK(touch_count(conn, run_id, "actual") == 2);
  // No declared rows were written by harvest.
  CHECK(touch_count(conn, run_id, "declared") == 0);
}

TEST_CASE("harvest: a clean worktree writes zero rows and returns zero", "[runs][harvest]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "z.txt", "1\n");
  REQUIRE(commit_all(repo.get(), "base"));

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto const run_id = seed_run(conn, "uid-clean");

  auto const n = rh::harvest(conn, rh::harvest_args{.worktree = repo.get(), .run_id = run_id, .task_id = 7});
  REQUIRE(n.has_value());
  CHECK(*n == 0);
}

TEST_CASE("harvest: a nonexistent worktree reports git_failed", "[runs][harvest]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto const run_id = seed_run(conn, "uid-nogit");

  auto const n = rh::harvest(conn, rh::harvest_args{
                                       .worktree = "/nonexistent/planar-harvest/path/xyzzy",
                                       .run_id   = run_id,
                                       .task_id  = 1,
                                   });
  REQUIRE_FALSE(n.has_value());
  CHECK(n.error() == rh::harvest_error::git_failed);
}

TEST_CASE("harvest: a run_id with no matching run row reports query_failed, not a silent write", "[runs][harvest]") {
  // The negative fixture for `touch_idempotent`'s FK constraint
  // (`run_touches.run_id references runs(id)`), which `insert or ignore`
  // does NOT swallow -- SQLite's IGNORE conflict resolution covers UNIQUE,
  // NOT NULL, PRIMARY KEY and CHECK, but not FOREIGN KEY, so a diff against
  // a run id that was never inserted fails loudly through `harvest` rather
  // than silently writing nothing. Exercises the branch a PERMISSIVE
  // mutation of `harvest`'s `if (!written.has_value())` guard (i.e. one
  // that always treats the write as successful) would otherwise hide: with
  // that guard neutered, this case would report `n == 1` instead of
  // `query_failed`, and no OTHER case in this file or in
  // `runs_leaves.t.cpp` reaches this branch at all.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "orphan.txt", "1\n");
  REQUIRE(commit_all(repo.get(), "base"));
  write_file(repo.get(), "orphan.txt", "2\n");

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  // Deliberately NO `seed_run` call: run id 999 does not exist in `runs`.

  auto const n = rh::harvest(conn, rh::harvest_args{.worktree = repo.get(), .run_id = 999, .task_id = 1});
  REQUIRE_FALSE(n.has_value());
  CHECK(n.error() == rh::harvest_error::query_failed);

  // And nothing was written -- the whole point of NOT swallowing the error.
  auto stmt = conn.prepare("select count(*) from run_touches");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);
}

TEST_CASE("harvest: idempotent -- a second harvest of an unchanged diff does not duplicate rows", "[runs][harvest]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const repo;
  REQUIRE(init_repo(repo.get()));
  write_file(repo.get(), "idem.txt", "v1\n");
  REQUIRE(commit_all(repo.get(), "base"));
  write_file(repo.get(), "idem.txt", "v2\n");

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_bare_plan(conn);
  auto const run_id = seed_run(conn, "uid-idem");

  auto const n1 = rh::harvest(conn, rh::harvest_args{.worktree = repo.get(), .run_id = run_id, .task_id = 99});
  REQUIRE(n1.has_value());
  CHECK(*n1 == 1);

  auto const n2 = rh::harvest(conn, rh::harvest_args{.worktree = repo.get(), .run_id = run_id, .task_id = 99});
  REQUIRE(n2.has_value());
  CHECK(*n2 == 1);

  CHECK(touch_count(conn, run_id, "actual") == 1);
}
