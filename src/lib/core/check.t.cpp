/// @file check.t.cpp
/// @brief planar.core.check — the guard survives NDEBUG (task 6347,
/// decision 1040).
///
/// THIS TU IS DELIBERATELY COMPILED AS IF IT WERE THE RELEASE BUILD.
/// `NDEBUG` is defined below, before any include, so `assert()` in this file
/// would expand to nothing. `check()` is an inline function in the module
/// interface and is therefore instantiated HERE, under that same `NDEBUG` —
/// which is what makes the death test below discriminating rather than
/// decorative. A `check(true)` call proves nothing on its own: it looks
/// identical whether the guard is live or deleted. Only observing the FAILING
/// path abort under `NDEBUG` distinguishes the two.

#define NDEBUG 1

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

import planar.core.check;
import std;

namespace {

/// @brief Run `body` in a forked child and report how the child died.
///
/// Catch2 has no death-test facility, and the failing path calls
/// `std::abort()` — which would take the whole test binary with it. The fork
/// is the only way to observe that abort and keep reporting.
///
/// @param body Callable run in the child.
/// @return The raw `waitpid` status.
auto status_after(const std::function<void()>& body) -> int {
  auto const pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // Child. stderr is closed so the expected diagnostic does not interleave
    // with Catch2's own output on a passing run.
    ::close(STDERR_FILENO);
    body();
    ::_exit(0); // reached only if body() did NOT abort — the survivor case
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return status;
}

} // namespace

TEST_CASE("check passes a true invariant through without aborting", "[core][check]") {
  auto const status = status_after([] { check(true, "true"); });
  INFO("raw waitpid status: " << status);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("check ABORTS on a false invariant even though this TU defines NDEBUG", "[core][check]") {
  // The property decision 1040 exists for. Under NDEBUG an `assert(false)`
  // here would fall through and the child would exit 0 — indistinguishable
  // from the passing case above. `check` must instead die on SIGABRT.
#ifndef NDEBUG
  FAIL("NDEBUG is not defined in this TU; the case cannot prove what it claims");
#endif
  auto const status = status_after([] { check(false, "deliberately false"); });
  INFO("raw waitpid status: " << status);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
}

TEST_CASE("check evaluates its condition rather than its expression text", "[core][check]") {
  // Guards against a rewrite that reads the diagnostic string instead of the
  // bool — the text here is the opposite of the condition on purpose.
  auto const status = status_after([] {
    auto const values = std::vector<int>{1, 2, 3};
    auto const summed = std::accumulate(values.begin(), values.end(), 0);
    check(summed == 6, "summed != 6");
  });
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}
