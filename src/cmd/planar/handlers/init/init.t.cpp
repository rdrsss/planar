// @file init.t.cpp
// @brief The `git` probe behind `planar init`'s `projects.git_remote`
// column, tested directly (plan 996, task 6132).
//
// ## Why this is its own translation unit
//
// `handlers.t.cpp` cannot import `planar.cmd.planar.handlers.init`. That
// module opens `namespace planar::cmd::handlers`, and `handlers.t.cpp`'s
// dispatch helper calls the FUNCTION `planar::cmd::make_handler_table(*tree)` — with
// both in scope the qualified name is ambiguous and the TU does not compile.
// Nothing about the split is cosmetic: it is the one arrangement in which
// both the end-to-end handler cases and these direct ones can exist.
//
// ## Why the probe is tested directly at all
//
// Every one of its arms is a NEGATIVE, and not one of them is observable
// from the verb's stdout. `init` in a directory with no `origin` produces
// byte-identical output to `init` in a directory the handler never probed —
// which is exactly the task-6128 shape, where `planar capture session`
// matched the oracle's bytes while writing NULL columns the engine
// supported. The positive arm is covered end-to-end in `handlers.t.cpp`
// (`init writes the git remote into the row and the JSON`), against the
// database ROW rather than against stdout; these are the four ways to
// answer "no" that a plausible implementation gets wrong.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

import std;
import planar.cmd.planar.handlers.init;

namespace {

/// @brief A unique scratch directory for one case.
/// @param tag A short discriminator so a failure names its own case.
/// @return The created directory.
auto scratch(std::string_view tag) -> std::filesystem::path {
  auto const      dir = std::filesystem::temp_directory_path() /
                        std::format("planar_init_probe_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

} // namespace

TEST_CASE("probe_git_origin answers nothing for every non-repository shape", "[cmd][handlers][init][6128]") {
  // The negative arms, directly. None of them is observable from stdout, and
  // all four are places a plausible implementation goes wrong: a `.git/config`
  // parser would answer for the second, and an implementation that ignored
  // git's exit status would answer an empty string for the first.
  auto const proj = scratch("probe");

  // (1) a plain directory
  CHECK_FALSE(planar::cmd::handlers::probe_git_origin(proj).has_value());

  // (2) a `.git` directory carrying a hand-written config and nothing else.
  // git refuses this ("not a git repository"); a config parser would not.
  std::error_code ec;
  std::filesystem::create_directories(proj / ".git", ec);
  {
    std::ofstream config(proj / ".git" / "config", std::ios::binary);
    config << "[remote \"origin\"]\n\turl = https://fake.example/x.git\n";
  }
  CHECK_FALSE(planar::cmd::handlers::probe_git_origin(proj).has_value());

  // (3) a path that does not exist at all
  CHECK_FALSE(planar::cmd::handlers::probe_git_origin(proj / "no" / "such" / "dir").has_value());

  // (4) a real repository with no remotes
  std::filesystem::remove_all(proj / ".git", ec);
  auto const dir = (proj).string();
  if (std::system(std::format("git -C '{}' init -q . >/dev/null 2>&1", dir).c_str()) == 0) {
    CHECK_FALSE(planar::cmd::handlers::probe_git_origin(proj).has_value());
  }
}

TEST_CASE("probe_git_origin reads origin, trims it, and ignores other remotes", "[cmd][handlers][init][6128]") {
  auto const proj = scratch("positive");
  auto const dir  = proj.string();
  if (std::system(std::format("git -C '{}' init -q . >/dev/null 2>&1", dir).c_str()) != 0) {
    SKIP("git unavailable");
  }

  // `upstream` alone answers nothing: the oracle asks for `origin` by name,
  // so "the first remote" and "any remote" are both wrong.
  REQUIRE(std::system(std::format("git -C '{}' remote add upstream https://example.com/up.git >/dev/null 2>&1", dir).c_str()) ==
          0);
  CHECK_FALSE(planar::cmd::handlers::probe_git_origin(proj).has_value());

  REQUIRE(std::system(
              std::format("git -C '{}' remote add origin git@github.com:example/repo.git >/dev/null 2>&1", dir).c_str()) == 0);
  auto const found = planar::cmd::handlers::probe_git_origin(proj);
  REQUIRE(found.has_value());
  // git terminates its answer with a newline; the stored column must not.
  CHECK(*found == "git@github.com:example/repo.git");
  CHECK_FALSE(found->contains('\n'));
}

TEST_CASE("probe_git_origin rejects output that came with a non-zero exit", "[cmd][handlers][init][6128]") {
  // THE ARM A FIXTURE CANNOT REACH WITH REAL GIT. Real `git remote get-url`
  // writes its failures to stderr and nothing to stdout, so the
  // empty-output check subsumes the exit-status check for every shape the
  // other cases can build — a first break-probe run confirmed exactly that:
  // deleting the `WEXITSTATUS(status) != 0` arm SURVIVED the whole suite.
  //
  // The two are NOT equivalent, and the oracle checks the status first
  // (zig/src/cmd/planar/handlers/init.zig's `gitRemoteOrigin` frees the
  // stdout and returns null on any non-`.exited`-0 term). So this case
  // builds the discriminating shape directly: a `git` that prints a
  // plausible URL to stdout AND exits non-zero. A handler that trusted
  // stdout would store it.
  //
  // Mutating PATH is safe here and only here: `catch_discover_tests` runs
  // each TEST_CASE as its own process, so the change cannot leak into
  // another case, and it is restored before returning regardless.
  auto const bin = scratch("fakegit");
  {
    std::ofstream fake(bin / "git", std::ios::binary);
    fake << "#!/bin/sh\necho 'https://stdout.example/should-not-be-stored.git'\nexit 1\n";
  }
  std::filesystem::permissions(bin / "git", std::filesystem::perms::owner_all, std::filesystem::perm_options::add);

  char const* const original = std::getenv("PATH");
  std::string const saved    = original == nullptr ? std::string{} : std::string{original};
  REQUIRE(::setenv("PATH", (bin.string() + ":" + saved).c_str(), 1) == 0);

  auto const answered = planar::cmd::handlers::probe_git_origin(bin);

  // Restore before asserting, so a failure does not leave the process with
  // a doctored PATH for Catch2's own teardown.
  if (saved.empty()) {
    ::unsetenv("PATH");
  } else {
    ::setenv("PATH", saved.c_str(), 1);
  }

  CHECK_FALSE(answered.has_value());
}
