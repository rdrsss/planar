// @file sessioncommits.t.cpp
// @brief Unit tests for `planar.engine.runtime.sessioncommits` (plan 996,
// task 6262).
//
// The module has ONE function and these tests pin the three things about it
// that are not readable off its signature:
//
//   * THE SORT COLUMN IS `recorded_at`, AND THE RENDERED COLUMN IS NOT.
//     `audit trail` displays `committed_at` when present. The fixture below
//     therefore writes two rows whose `recorded_at` and `committed_at`
//     orders DISAGREE, so an implementation that sorted by the column the
//     operator sees fails here rather than looking right in a hand-check.
//   * AN EMPTY `session_ids` IS AN ANSWER, NOT AN ERROR. Asserted against a
//     connection whose table HAS rows, so an implementation that ignored
//     the predicate would return those rows and fail here.
//
//     READ THIS BEFORE STRENGTHENING THAT CASE. The first draft of it
//     claimed to pin the early-return guard, on the reasoning that the
//     `in ()` it would otherwise compose is a SQLite syntax error. IT IS
//     NOT — SQLite accepts an empty `IN ()` list and evaluates it to false
//     (verified directly: `select count(*) from t where a in ()` returns 0
//     at exit 0). The break-probe that deletes the guard is therefore a
//     SURVIVOR, and is recorded as one rather than being made to look like
//     a kill. What this case pins is the CONTRACT (empty in, empty out, no
//     failure), which a mutant returning `query_failed` for the empty case
//     does kill — that probe was run and is a kill.
//   * `limit` CAPS AFTER THE SORT, so a cap of 1 returns the newest-recorded
//     row rather than an arbitrary one. Asserted by identity, not by count.
//
// Rows go in as raw SQL for the READ-half tests above because nothing in
// this tree used to WRITE `session_commits`. Task 6358 changed that:
// `resolve_repo_root_strict` / `walk_strict` / `resolve_shas` /
// `record_count`, below, are the git-walk WRITE half `capture commits`
// needs, tested here against REAL hermetic git fixtures (never the Planar
// checkout) rather than against a stub -- a fake `git` would have passed
// against the embedded-NUL truncation bug this pass actually found (see
// the multi-commit cases' header note).

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.sessioncommits;

namespace {

namespace sc = planar::engine::runtime::sessioncommits;

/// @brief A unique scratch directory tree, removed when the guard goes out
/// of scope. Copied from git.t.cpp / capture.t.cpp so these fixture repos
/// never touch the Planar checkout.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_sessioncommits_git_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] auto get() const -> const std::filesystem::path& {
    return path_;
  }
};

/// @brief Run a shell line inside `dir`, for FIXTURE construction only.
/// @param dir The working directory.
/// @param line The shell line.
/// @return True when the line exited 0.
auto fixture_sh(const std::filesystem::path& dir, std::string_view line) -> bool {
  std::string const composed = std::format("cd '{}' && {} >/dev/null 2>&1", dir.string(), line);
  return std::system(composed.c_str()) == 0;
}

/// @brief Is `git` runnable at all on this machine?
auto have_git() -> bool {
  static bool const answer = std::system("git --version >/dev/null 2>&1") == 0;
  return answer;
}

/// @brief `git rev-parse HEAD` inside `dir`, trimmed.
/// @param dir The repository.
/// @return The sha, or empty on any failure.
auto head_sha(const std::filesystem::path& dir) -> std::string {
  std::string const cmd  = std::format("cd '{}' && git rev-parse HEAD", dir.string());
  FILE*             pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return {};
  }
  std::string sha;
  char        buf[256];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    sha += buf;
  }
  pclose(pipe);
  while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) {
    sha.pop_back();
  }
  return sha;
}

/// @brief Build a hermetic repository at `dir` with one commit.
/// @param dir An existing empty directory.
/// @return True when every fixture step succeeded.
auto make_repo(const std::filesystem::path& dir) -> bool {
  return fixture_sh(dir, "git init -q -b main") && fixture_sh(dir, "git config user.email planar@example.invalid") &&
         fixture_sh(dir, "git config user.name Planar") && fixture_sh(dir, "git commit -q --allow-empty -m base");
}

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_sessioncommits_test_{}_{}.db",
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

/// @brief Two sessions; three commits whose `recorded_at` order is the
/// REVERSE of their `committed_at` order.
///
/// sha `early-rec` was committed LAST but recorded FIRST, and `late-rec`
/// the other way round. Any implementation sorting on `committed_at`
/// returns them in the opposite order to the one asserted below.
auto seed(planar::db::connection& conn) -> void {
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into sessions (id, vendor, started_at) values (2, 'codex', '2026-01-01T00:00:00.000Z')");

  exec(conn, "insert into session_commits (id, session_id, sha, subject, committed_at, recorded_at) "
             "values (1, 1, 'early-rec', 'committed last, recorded first', "
             "'2026-03-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into session_commits (id, session_id, sha, subject, committed_at, recorded_at) "
             "values (2, 1, 'late-rec', 'committed first, recorded last', "
             "'2026-02-01T00:00:00.000Z', '2026-02-01T00:00:00.000Z')");
  // Belongs to the OTHER session, so a query that ignored its `in (…)`
  // predicate would pick it up.
  exec(conn, "insert into session_commits (id, session_id, sha, recorded_at) "
             "values (3, 2, 'other-session', '2026-04-01T00:00:00.000Z')");
}

} // namespace

TEST_CASE("list_for_sessions orders by recorded_at, not by the committed_at it renders", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  std::array<std::int64_t, 1> ids{1};
  auto const                  rows = sc::list_for_sessions(conn, ids, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  // Newest RECORDED first. `late-rec` has the later `recorded_at` and the
  // EARLIER `committed_at`; a committed_at sort would put `early-rec` here.
  CHECK((*rows)[0].sha == "late-rec");
  CHECK((*rows)[1].sha == "early-rec");
  // The other session's row is absent — the predicate is doing work.
  CHECK((*rows)[0].session_id == 1);
  CHECK((*rows)[1].session_id == 1);
}

TEST_CASE("list_for_sessions caps after sorting, so limit 1 keeps the newest-recorded row", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  std::array<std::int64_t, 1> ids{1};
  auto const                  rows = sc::list_for_sessions(conn, ids, 1);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  // By identity, not by count: a cap applied before the sort would also
  // return exactly one row.
  CHECK((*rows)[0].sha == "late-rec");
}

TEST_CASE("list_for_sessions spans every id it is given", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  std::array<std::int64_t, 2> ids{1, 2};
  auto const                  rows = sc::list_for_sessions(conn, ids, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 3);
  // Session 2's row was recorded last, so it leads.
  CHECK((*rows)[0].sha == "other-session");
  CHECK((*rows)[1].sha == "late-rec");
  CHECK((*rows)[2].sha == "early-rec");
}

TEST_CASE("list_for_sessions answers an empty id list without running a query", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The table HAS rows here on purpose: an implementation that ignored the
  // id predicate would return them and fail. It does NOT discriminate the
  // early-return guard — see this file's header for why that probe survives
  // and why it is reported rather than hidden.
  std::array<std::int64_t, 0> none{};
  auto const                  rows = sc::list_for_sessions(conn, none, std::nullopt);
  REQUIRE(rows.has_value());
  CHECK(rows->empty());
}

TEST_CASE("list_for_sessions returns every optional column, set and unset", "[engine][sessioncommits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, status, "
             "vendor, claimed_at, last_heartbeat_at, lease_expires_at) "
             "values (7, 'tok', 1, 'task', 1, 'exclusive', 'active', 'claude', "
             "'2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', '2026-01-01T01:00:00.000Z')");
  exec(conn, "insert into session_commits (id, session_id, claim_id, sha, repo_root, branch, subject, author, "
             "committed_at, recorded_at) values (1, 1, 7, 'full', '/repo', 'main', 'subject text', 'Ada', "
             "'2026-02-01T00:00:00.000Z', '2026-02-01T00:00:01.000Z')");
  exec(conn, "insert into session_commits (id, session_id, sha, recorded_at) "
             "values (2, 1, 'bare', '2026-01-01T00:00:00.000Z')");

  std::array<std::int64_t, 1> ids{1};
  auto const                  rows = sc::list_for_sessions(conn, ids, std::nullopt);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);

  // ABSENCE IS ASSERTED ALONGSIDE PRESENCE. A decode that hard-coded every
  // optional to `nullopt` would satisfy the `bare` row alone, so the `full`
  // row pins the other side of each field.
  auto const& full = (*rows)[0];
  CHECK(full.sha == "full");
  CHECK(full.claim_id == 7);
  CHECK(full.repo_root == "/repo");
  CHECK(full.branch == "main");
  CHECK(full.subject == "subject text");
  CHECK(full.author == "Ada");
  CHECK(full.committed_at == "2026-02-01T00:00:00.000Z");
  CHECK(full.recorded_at == "2026-02-01T00:00:01.000Z");

  auto const& bare = (*rows)[1];
  CHECK(bare.sha == "bare");
  CHECK_FALSE(bare.claim_id.has_value());
  CHECK_FALSE(bare.repo_root.has_value());
  CHECK_FALSE(bare.branch.has_value());
  CHECK_FALSE(bare.subject.has_value());
  CHECK_FALSE(bare.author.has_value());
  CHECK_FALSE(bare.committed_at.has_value());
  CHECK(bare.recorded_at == "2026-01-01T00:00:00.000Z");
}

// ---------------------------------------------------------------------------
// resolve_repo_root_strict / walk_strict / resolve_shas / record_count
// (task 6358)
// ---------------------------------------------------------------------------

TEST_CASE("resolve_repo_root_strict resolves a real repository and refuses a non-repository",
          "[engine][sessioncommits][commits]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));

  auto const root = sc::resolve_repo_root_strict(repo.get());
  REQUIRE(root.has_value());
  CHECK(std::filesystem::canonical(*root) == std::filesystem::canonical(repo.get()));

  scratch_dir nongit;
  auto const  failed = sc::resolve_repo_root_strict(nongit.get());
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error() == sc::commits_error::not_git);
}

TEST_CASE("walk_strict returns MULTIPLE commits newest-first with every field populated", "[engine][sessioncommits][commits]") {
  // THE MULTI-COMMIT CASE, AND A MEASURED EQUIVALENT MUTANT. `git log -z`
  // NUL-terminates both each FIELD (our own `--format` string ends in
  // `%x00`) and each COMMIT's whole record, so the raw bytes between two
  // commits carry a DOUBLE NUL (confirmed by hex-dumping a real 2-commit
  // `git log -z` run). `parse_commits`'s leading-NUL skip is therefore a
  // `while`, not an `if` -- but a break-probe against exactly this
  // fixture found `while` -> `if` a SURVIVOR: by the time the loop returns
  // to skip leading NULs for the next record, field 4's OWN terminator has
  // already consumed ONE of the two, leaving exactly one for the skip to
  // eat, so `if` and `while` are observably IDENTICAL for this git-emitted
  // shape. That is a genuine equivalent mutant, not a coverage gap --
  // `while` is still the correct general contract (an empty trailing field
  // would legitimately produce a longer NUL run), and is kept for that
  // reason, not because this fixture can prove it.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  auto const base = head_sha(repo.get());
  REQUIRE_FALSE(base.empty());

  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m first"));
  auto const first_sha = head_sha(repo.get());
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m second"));
  auto const second_sha = head_sha(repo.get());

  auto const commits = sc::walk_strict(repo.get(), base);
  REQUIRE(commits.has_value());
  REQUIRE(commits->size() == 2);

  // Newest first: `second` precedes `first`.
  CHECK((*commits)[0].sha == second_sha);
  CHECK((*commits)[0].subject == "second");
  CHECK((*commits)[1].sha == first_sha);
  CHECK((*commits)[1].subject == "first");

  // Every field on EVERY row, not just the first -- the misalignment a
  // broken leading-NUL skip produces shows up on the SECOND row's fields,
  // not the first's.
  for (auto const& commit : *commits) {
    CHECK_FALSE(commit.sha.empty());
    CHECK(commit.repo_root.has_value());
    CHECK(commit.branch == "main");
    CHECK(commit.author == "Planar");
    CHECK(commit.committed_at.has_value());
  }
}

TEST_CASE("walk_strict returns an empty vector for a valid range with no commits", "[engine][sessioncommits][commits]") {
  // THE NO-NEW-COMMITS PATH. `git log -z` on an empty range answers exit 0
  // with EMPTY stdout -- success, not `git_failed`.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  auto const head = head_sha(repo.get());
  REQUIRE_FALSE(head.empty());

  auto const commits = sc::walk_strict(repo.get(), head);
  REQUIRE(commits.has_value());
  CHECK(commits->empty());
}

TEST_CASE("walk_strict surfaces git_failed for an unresolvable ref", "[engine][sessioncommits][commits]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));

  auto const commits = sc::walk_strict(repo.get(), "not-a-ref");
  REQUIRE_FALSE(commits.has_value());
  CHECK(commits.error() == sc::commits_error::git_failed);
}

TEST_CASE("resolve_shas resolves MULTIPLE explicit SHAs in the REQUESTED order, not commit order",
          "[engine][sessioncommits][commits]") {
  // The requested order deliberately DISAGREES with commit order: `first`
  // is requested before `second` even though `second` was committed
  // later. A resolver that re-sorted by commit time would fail here.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m first"));
  auto const first_sha = head_sha(repo.get());
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m second"));
  auto const second_sha = head_sha(repo.get());

  std::array<std::string_view, 2> const shas{first_sha, second_sha};
  auto const                            commits = sc::resolve_shas(repo.get(), shas);
  REQUIRE(commits.has_value());
  REQUIRE(commits->size() == 2);
  CHECK((*commits)[0].sha == first_sha);
  CHECK((*commits)[0].subject == "first");
  CHECK((*commits)[1].sha == second_sha);
  CHECK((*commits)[1].subject == "second");
}

TEST_CASE("resolve_shas returns an empty vector without spawning git when given no SHAs", "[engine][sessioncommits][commits]") {
  scratch_dir repo;
  auto const  commits = sc::resolve_shas(repo.get(), {});
  REQUIRE(commits.has_value());
  CHECK(commits->empty());
}

TEST_CASE("resolve_shas surfaces git_failed for an unresolvable SHA", "[engine][sessioncommits][commits]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));

  std::array<std::string_view, 1> const shas{"not-a-real-sha"};
  auto const                            commits = sc::resolve_shas(repo.get(), shas);
  REQUIRE_FALSE(commits.has_value());
  CHECK(commits.error() == sc::commits_error::git_failed);
}

TEST_CASE("record_count inserts every commit once and reports zero on a duplicate re-run", "[engine][sessioncommits][commits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");

  std::array<sc::commit_meta, 2> const commits{
      sc::commit_meta{.sha          = "sha-a",
                      .repo_root    = "/repo",
                      .branch       = "main",
                      .subject      = "a",
                      .author       = "Ada",
                      .committed_at = "2026-01-01T00:00:00Z"},
      sc::commit_meta{.sha          = "sha-b",
                      .repo_root    = "/repo",
                      .branch       = "main",
                      .subject      = "b",
                      .author       = "Ada",
                      .committed_at = "2026-01-01T00:00:01Z"},
  };

  auto const first = sc::record_count(conn, 1, std::nullopt, commits);
  REQUIRE(first.has_value());
  CHECK(*first == 2);

  // Re-run: the `(session_id, sha)` unique constraint makes it idempotent,
  // not a caller-side dedupe -- `record_count` itself must return zero.
  auto const second = sc::record_count(conn, 1, std::nullopt, commits);
  REQUIRE(second.has_value());
  CHECK(*second == 0);

  auto const rows = sc::list_filtered(conn, sc::list_filter{.session_id = 1});
  REQUIRE(rows.has_value());
  CHECK(rows->size() == 2);
}

// ---------------------------------------------------------------------------
// `walk` — the FAIL-SOFT variant (task 6360)
// ---------------------------------------------------------------------------

TEST_CASE("walk returns the same commits walk_strict would, for a valid range", "[engine][sessioncommits][commits]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m first"));
  auto const base = head_sha(repo.get());
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m second"));

  auto const commits = sc::walk(repo.get(), base);
  REQUIRE(commits.size() == 1);
  CHECK(commits[0].subject == "second");
}

TEST_CASE("walk degrades an unresolvable ref to an empty vector instead of an error", "[engine][sessioncommits][commits]") {
  // The whole point of the fail-soft variant: `walk_strict` on the same
  // input surfaces `git_failed` (see the case above); `walk` must not
  // propagate that, or its caller (`capture::close_session`'s automatic
  // harvest) would fail the whole verb on a git hiccup.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));

  auto const commits = sc::walk(repo.get(), "not-a-ref");
  CHECK(commits.empty());
}

TEST_CASE("walk degrades a non-repository directory to an empty vector", "[engine][sessioncommits][commits]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir non_repo;
  auto const  commits = sc::walk(non_repo.get(), "HEAD");
  CHECK(commits.empty());
}

// ---------------------------------------------------------------------------
// `record_claim_window_best_effort` — the `planar-agent` terminal-verb
// claim-window fold (task 6360)
// ---------------------------------------------------------------------------

TEST_CASE("record_claim_window_best_effort records commits reachable from worktree_path", "[engine][sessioncommits][commits]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  // `session_commits.claim_id` FK-references `agent_work_claims(id)` -- a
  // dangling id is silently REJECTED by the insert (task 6360's own
  // best-effort contract swallows that failure), so the row must exist for
  // this to test the fold rather than a foreign-key rejection.
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, status, "
             "vendor, claimed_at, last_heartbeat_at, lease_expires_at) "
             "values (42, 'tok-42', 1, 'task', 1, 'exclusive', 'active', 'claude', "
             "'2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', '2026-01-01T01:00:00.000Z')");

  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  auto const base = head_sha(repo.get());
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m claimed"));

  std::string const repo_str = repo.get().string();
  sc::record_claim_window_best_effort(conn,
                                      sc::claim_window{
                                          .claim_id          = 42,
                                          .session_id        = 1,
                                          .worktree_path     = repo_str,
                                          .repo_root         = repo_str,
                                          .head_sha_at_claim = base,
                                      },
                                      /*no_locality_probe=*/false);

  auto const rows = sc::list_filtered(conn, sc::list_filter{.session_id = 1});
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  CHECK((*rows)[0].subject == "claimed");
  REQUIRE((*rows)[0].claim_id.has_value());
  CHECK(*(*rows)[0].claim_id == 42);
}

TEST_CASE("record_claim_window_best_effort is a no-op when no_locality_probe is set", "[engine][sessioncommits][commits]") {
  // PERMISSIVE probe target: a mutation that ALWAYS skipped the probe
  // (dropped `--no-locality-probe` entirely) would still pass every OTHER
  // case here, since none of them assert on the flag being HONOURED in the
  // negative direction except this one.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");

  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  auto const base = head_sha(repo.get());
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m claimed"));

  std::string const repo_str = repo.get().string();
  sc::record_claim_window_best_effort(conn,
                                      sc::claim_window{
                                          .claim_id          = 42,
                                          .session_id        = 1,
                                          .worktree_path     = repo_str,
                                          .repo_root         = repo_str,
                                          .head_sha_at_claim = base,
                                      },
                                      /*no_locality_probe=*/true);

  auto const rows = sc::list_filtered(conn, sc::list_filter{.session_id = 1});
  REQUIRE(rows.has_value());
  CHECK(rows->empty());
}

TEST_CASE("record_claim_window_best_effort is a no-op when head_sha_at_claim is unset", "[engine][sessioncommits][commits]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");

  sc::record_claim_window_best_effort(conn,
                                      sc::claim_window{
                                          .claim_id   = 42,
                                          .session_id = 1,
                                      },
                                      /*no_locality_probe=*/false);

  auto const rows = sc::list_filtered(conn, sc::list_filter{.session_id = 1});
  REQUIRE(rows.has_value());
  CHECK(rows->empty());
}

TEST_CASE("record_claim_window_best_effort falls back to repo_root when the worktree_path walk fails",
          "[engine][sessioncommits][commits]") {
  // The oracle's `walkClaimWindow`: a STRICT walk over `worktree_path`
  // that fails falls back to the FAIL-SOFT `walk` over `repo_root`, but
  // ONLY because the two directories differ here -- see the next case for
  // the guard against re-walking the same directory twice.
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, status, "
             "vendor, claimed_at, last_heartbeat_at, lease_expires_at) "
             "values (42, 'tok-42', 1, 'task', 1, 'exclusive', 'active', 'claude', "
             "'2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z', '2026-01-01T01:00:00.000Z')");

  scratch_dir repo;
  REQUIRE(make_repo(repo.get()));
  auto const base = head_sha(repo.get());
  REQUIRE(fixture_sh(repo.get(), "git commit -q --allow-empty -m claimed"));

  scratch_dir non_repo; // stands in for a worktree that has since been removed.

  std::string const repo_str     = repo.get().string();
  std::string const non_repo_str = non_repo.get().string();
  sc::record_claim_window_best_effort(conn,
                                      sc::claim_window{
                                          .claim_id          = 42,
                                          .session_id        = 1,
                                          .worktree_path     = non_repo_str,
                                          .repo_root         = repo_str,
                                          .head_sha_at_claim = base,
                                      },
                                      /*no_locality_probe=*/false);

  auto const rows = sc::list_filtered(conn, sc::list_filter{.session_id = 1});
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  CHECK((*rows)[0].subject == "claimed");
}
