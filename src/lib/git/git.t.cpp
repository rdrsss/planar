// @file git.t.cpp
// @brief Unit tests for `planar.git` (plan 996, tasks 6128 and 6137).
//
// These build REAL repositories on disk and REAL linked worktrees, and
// assert on what `git` actually answers. That is deliberate and it is the
// point of the task these tests exist for: the class of defect being closed
// here — a probe that returns "no answer" where the oracle returns a value,
// and a gate that classifies a linked worktree as an ordinary checkout — is
// invisible to a stubbed subprocess. A fake `git` would have passed against
// the broken `<toplevel>/.git` comparison the module header describes.
//
// Every case that needs `git` skips itself when `git` is not on PATH rather
// than failing, so the suite stays runnable on a machine without it. The
// pure-string fast-path cases never skip.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.git;

namespace {

namespace git = planar::git;

/// @brief A unique scratch directory tree, removed when the guard goes out
/// of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_git_test_{}_{}",
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

  /// @brief The directory's path.
  /// @return The path.
  [[nodiscard]] auto get() const -> const std::filesystem::path& {
    return path_;
  }
};

/// @brief Is `git` runnable at all on this machine?
/// @return True when `git --version` exits 0.
auto have_git() -> bool {
  static constexpr std::array<std::string_view, 1> k_args{"--version"};
  static bool const                                answer = git::run(std::filesystem::current_path(), k_args).has_value();
  return answer;
}

/// @brief Run a shell line inside `dir`, aborting the test on failure.
///
/// Used only for FIXTURE construction (`git init`, `git commit`, `git
/// worktree add`), never for the behavior under test.
/// @param dir The working directory.
/// @param line The shell line.
/// @return True when the line exited 0.
auto fixture_sh(const std::filesystem::path& dir, std::string_view line) -> bool {
  std::string const composed = std::format("cd '{}' && {} >/dev/null 2>&1", dir.string(), line);
  return std::system(composed.c_str()) == 0;
}

/// @brief Build a repository with one commit at `dir`.
/// @param dir An existing empty directory.
/// @return True when every fixture step succeeded.
auto make_repo(const std::filesystem::path& dir) -> bool {
  return fixture_sh(dir, "git init -q -b main") && fixture_sh(dir, "git config user.email planar@example.invalid") &&
         fixture_sh(dir, "git config user.name Planar") && fixture_sh(dir, "touch seed.txt") &&
         fixture_sh(dir, "git add seed.txt") && fixture_sh(dir, "git commit -q -m seed");
}

} // namespace

// ---------------------------------------------------------------------------
// run / run_trimmed
// ---------------------------------------------------------------------------

TEST_CASE("run reports no answer for a directory that is not a repository", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const                                dir;
  static constexpr std::array<std::string_view, 2> k_args{"rev-parse", "HEAD"};
  CHECK_FALSE(git::run(dir.get(), k_args).has_value());
  CHECK_FALSE(git::run_trimmed(dir.get(), k_args).has_value());
}

TEST_CASE("run_trimmed strips the trailing newline git always emits", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));

  static constexpr std::array<std::string_view, 2> k_args{"rev-parse", "HEAD"};
  auto const                                       raw     = git::run(dir.get(), k_args);
  auto const                                       trimmed = git::run_trimmed(dir.get(), k_args);
  REQUIRE(raw.has_value());
  REQUIRE(trimmed.has_value());
  // The raw form carries git's newline; the trimmed one does not, and the
  // trimmed one is a full 40-character object name.
  CHECK(raw->size() == trimmed->size() + 1);
  CHECK(trimmed->size() == 40);
  CHECK(trimmed->find_first_of(" \t\r\n") == std::string::npos);
}

TEST_CASE("run_trimmed reports no answer rather than the empty string", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));

  // `git status --porcelain` on a clean tree exits 0 with EMPTY stdout.
  // That is the exact shape the "empty is not an answer" rule exists for:
  // exit 0, nothing to report. `run` sees the success; `run_trimmed` must
  // still report unset, NOT an engaged optional holding "".
  static constexpr std::array<std::string_view, 2> k_args{"status", "--porcelain"};
  auto const                                       raw = git::run(dir.get(), k_args);
  REQUIRE(raw.has_value());
  CHECK(raw->empty());
  CHECK_FALSE(git::run_trimmed(dir.get(), k_args).has_value());
}

TEST_CASE("run quotes an argument containing a space and a single quote", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));

  // A commit subject that would break an unquoted `/bin/sh` line twice
  // over. If the quoting were wrong this either fails to spawn or echoes
  // something else entirely; it cannot round-trip by accident.
  std::string const                     subject = "it's a subject with spaces";
  std::array<std::string_view, 4> const k_args{"log", "-1", "--format=%s", "HEAD"};
  REQUIRE(fixture_sh(dir.get(), std::format("git commit -q --allow-empty -m \"{}\"", subject)));
  auto const got = git::run_trimmed(dir.get(), k_args);
  REQUIRE(got.has_value());
  CHECK(*got == subject);

  // And the quoting applies to the `-C` path too: a directory whose name
  // contains a space is probed correctly.
  auto const spaced = dir.get() / "a dir with spaces";
  std::filesystem::create_directories(spaced);
  REQUIRE(make_repo(spaced));
  static constexpr std::array<std::string_view, 2> k_head{"rev-parse", "HEAD"};
  CHECK(git::run_trimmed(spaced, k_head).has_value());
}

// ---------------------------------------------------------------------------
// probe_start_context — the task 6128 primitive
// ---------------------------------------------------------------------------

TEST_CASE("probe_start_context answers with the toplevel and HEAD of a real repo", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));

  auto const ctx = git::probe_start_context(dir.get());
  REQUIRE(ctx.has_value());
  CHECK(ctx->head_sha_at_start.size() == 40);
  CHECK_FALSE(ctx->repo_root.empty());
  // The reported root IS this repository, compared canonically because
  // macOS's temp dir is itself a symlink (`/tmp` -> `/private/tmp`) and a
  // string compare would fail for a reason that has nothing to do with the
  // probe.
  CHECK(std::filesystem::canonical(ctx->repo_root) == std::filesystem::canonical(dir.get()));
}

TEST_CASE("probe_start_context reports unset when HEAD has no commit", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(fixture_sh(dir.get(), "git init -q -b main"));

  // A freshly-initialised repository ANSWERS `--show-toplevel` and REFUSES
  // `rev-parse HEAD`. Both columns are written together or not at all, so
  // this must be unset rather than a context with an empty sha — that
  // half-filled row is precisely the degraded shape task 6128 is about.
  static constexpr std::array<std::string_view, 2> k_toplevel{"rev-parse", "--show-toplevel"};
  CHECK(git::run_trimmed(dir.get(), k_toplevel).has_value());
  CHECK_FALSE(git::probe_start_context(dir.get()).has_value());
}

TEST_CASE("probe_start_context reports unset outside a repository", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  CHECK_FALSE(git::probe_start_context(dir.get()).has_value());
}

// ---------------------------------------------------------------------------
// probe_origin_url
// ---------------------------------------------------------------------------

TEST_CASE("probe_origin_url answers with the configured remote and unset without one", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));

  CHECK_FALSE(git::probe_origin_url(dir.get()).has_value());
  REQUIRE(fixture_sh(dir.get(), "git remote add origin https://example.invalid/planar.git"));
  auto const url = git::probe_origin_url(dir.get());
  REQUIRE(url.has_value());
  CHECK(*url == "https://example.invalid/planar.git");
}

// ---------------------------------------------------------------------------
// fast_path_worktree_root — pure string, no subprocess, never skipped
// ---------------------------------------------------------------------------

TEST_CASE("fast_path_worktree_root requires both slashes around .worktrees", "[lib][git]") {
  // No boundary at all.
  CHECK_FALSE(git::fast_path_worktree_root("/repo/src/cmd").has_value());
  // A directory literally NAMED `something.worktrees` — no leading slash
  // before `.worktrees`, so it must not match.
  CHECK_FALSE(git::fast_path_worktree_root("/repo/agent.worktrees/x").has_value());
  // `.worktrees/` with no segment after it.
  CHECK_FALSE(git::fast_path_worktree_root("/repo/.worktrees/").has_value());
}

TEST_CASE("fast_path_worktree_root stops at the first segment under .worktrees", "[lib][git]") {
  // Hand-picked single-segment convention.
  auto const one = git::fast_path_worktree_root("/repo/.worktrees/agent-1");
  REQUIRE(one.has_value());
  CHECK(*one == "/repo/.worktrees/agent-1");

  // The methodology's two-level `<plan>/<task>` convention, probed from
  // deep inside. The root reported is the FIRST segment; classification is
  // all the gate needs.
  auto const deep = git::fast_path_worktree_root("/repo/.worktrees/plan-996/task-6137/src/lib");
  REQUIRE(deep.has_value());
  CHECK(*deep == "/repo/.worktrees/plan-996");
}

// ---------------------------------------------------------------------------
// detect_worktree — the task 6137 primitive
// ---------------------------------------------------------------------------

TEST_CASE("detect_worktree classifies an ordinary checkout as NOT a worktree", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));

  auto const det = git::detect_worktree(dir.get());
  CHECK_FALSE(det.is_worktree);
  CHECK_FALSE(det.worktree_root.has_value());
}

TEST_CASE("detect_worktree classifies a checkout probed from several levels down", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  REQUIRE(make_repo(dir.get()));
  auto const deep = dir.get() / "a" / "b" / "c";
  std::filesystem::create_directories(deep);

  // The regression the `--path-format=absolute` note in the module header
  // describes: without it, `--git-common-dir` comes back relative to the
  // INVOCATION cwd, and resolving it against the toplevel produced a bogus
  // path that failed the equality check — reporting an ordinary checkout,
  // probed from two-or-more levels down, as a secondary worktree.
  auto const det = git::detect_worktree(deep);
  CHECK_FALSE(det.is_worktree);
}

TEST_CASE("detect_worktree classifies a REAL linked worktree as a worktree", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const root;
  auto const        main_repo = root.get() / "main";
  std::filesystem::create_directories(main_repo);
  REQUIRE(make_repo(main_repo));

  // A genuine `git worktree add` — NOT a path that merely looks like one.
  // The fast path cannot fire here (no `.worktrees/` segment), so this
  // exercises the authoritative git fallback end to end.
  auto const linked = root.get() / "linked";
  REQUIRE(fixture_sh(main_repo, std::format("git worktree add -q -b side '{}'", linked.string())));

  auto const det = git::detect_worktree(linked);
  REQUIRE(det.is_worktree);
  REQUIRE(det.worktree_root.has_value());
  REQUIRE(det.parent_repo_root.has_value());
  CHECK(std::filesystem::canonical(*det.worktree_root) == std::filesystem::canonical(linked));
  CHECK(std::filesystem::canonical(*det.parent_repo_root) == std::filesystem::canonical(main_repo));

  // And from a SUBDIRECTORY of the worktree, which is where an operator
  // actually stands. The reported roots are unchanged.
  auto const inside = linked / "nested";
  std::filesystem::create_directories(inside);
  auto const nested_det = git::detect_worktree(inside);
  REQUIRE(nested_det.is_worktree);
  CHECK(std::filesystem::canonical(*nested_det.worktree_root) == std::filesystem::canonical(linked));
}

TEST_CASE("detect_worktree classifies a SUBMODULE checkout as NOT a worktree", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const root;
  auto const        upstream = root.get() / "upstream";
  auto const        super    = root.get() / "super";
  std::filesystem::create_directories(upstream);
  std::filesystem::create_directories(super);
  REQUIRE(make_repo(upstream));
  REQUIRE(make_repo(super));

  if (!fixture_sh(super, std::format("git -c protocol.file.allow=always submodule add -q '{}' sub", upstream.string()))) {
    SKIP("this git refuses local-path submodules");
  }

  // A submodule's common dir is `<super>/.git/modules/sub` — never
  // `<toplevel>/.git`. The older rule ("common dir == <toplevel>/.git")
  // therefore reported EVERY submodule as a secondary worktree and refused
  // planning verbs inside it. Its git dir and common dir are the same
  // directory, which is what makes the current rule get it right.
  auto const det = git::detect_worktree(super / "sub");
  CHECK_FALSE(det.is_worktree);
}

TEST_CASE("detect_worktree reports not-a-worktree outside a repository", "[lib][git]") {
  if (!have_git()) {
    SKIP("git not on PATH");
  }
  scratch_dir const dir;
  // Total by contract: a detection failure must never refuse a planning
  // verb, so "git said nothing" is "not a worktree".
  CHECK_FALSE(git::detect_worktree(dir.get()).is_worktree);
}

TEST_CASE("detect_worktree fast path fires without git needing to agree", "[lib][git]") {
  scratch_dir const dir;
  auto const        conventional = dir.get() / ".worktrees" / "plan-996" / "task-6137";
  std::filesystem::create_directories(conventional);

  // No repository anywhere here. The orchestrator's `.worktrees/`
  // convention classifies on the PATH alone, which is what lets the gate
  // fire before any git call and on a machine without git installed.
  auto const det = git::detect_worktree(conventional);
  REQUIRE(det.is_worktree);
  REQUIRE(det.worktree_root.has_value());
  REQUIRE(det.parent_repo_root.has_value());
  CHECK(*det.worktree_root == (dir.get() / ".worktrees" / "plan-996").string());
  CHECK(*det.parent_repo_root == dir.get().string());
}
