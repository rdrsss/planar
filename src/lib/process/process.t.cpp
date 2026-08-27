// @file process.t.cpp
// @brief Tests for `planar.process` (plan 996, task 6272).
//
// ## A spawn seam that is green because nothing ever spawns
//
// That is the silent-degradation shape this milestone keeps finding, and
// this module is its worst case: a spawn writes no database state, so the
// state differential cannot catch it either. A `run_inherited` that
// returned `0` without forking, and a `capture` that returned
// `{spawned=true, exit_code=0, output=""}` without spawning, would both
// satisfy a naive "the call succeeded" assertion.
//
// So every spawn here is witnessed THREE independent ways, following the
// pattern the editflow cycle established for `$EDITOR`:
//
//   1. **argv** — the stub records its own `"$@"` to a witness file, and
//      the test asserts the exact argument vector it received. A handler
//      that builds the wrong argv fails here even though the spawn
//      happened.
//   2. **A provenance sentinel** — `k_sentinel` below appears NOWHERE in
//      `src/`, so it cannot be produced by anything except this stub
//      actually running. A faked success cannot invent it. (Deliberately
//      not derived from any constant under test: it is a fixed literal
//      owned by this file.)
//   3. **Exit-code handling** — the stub exits with a code the test chose,
//      and the test asserts the value came back. A seam that always
//      reports 0 fails here.
//
// ## Break-probe (task 6272)
//
// Replacing `run_inherited`'s `fork`/`execv` with a bare `return 0`:
//
//   FAILED: run_inherited executes the program and its argv arrives intact
//   FAILED: run_inherited propagates a non-zero exit status exactly
//   FAILED: run_inherited reports signal death as 1
//   FAILED: run_inherited passes every argument through untouched
//
// Four NAMED tests die, on all three witnesses independently. Doing the
// same to `capture` (returning `{.spawned = true}` without spawning):
//
//   FAILED: capture runs a REAL subprocess and separates its three outcomes
//   FAILED: capture returns the child's stdout verbatim, untrimmed
//
// ## The one probe that SURVIVES, and why it stays
//
// Deleting the `rc != 0` early return in `capture` leaves every test here
// GREEN. That is a coincidental equivalence, not dead code: with no other
// children, `waitpid(0, ...)` returns ECHILD, which this code also maps to
// `spawned = false`. The equivalence breaks the moment the process has
// another child — `waitpid(0, ...)` reaps ANY child in the process group
// and would report an unrelated child's status as this command's. The
// guard is kept deliberately; see the comment at the site in `process.cpp`.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <unistd.h>

import std;
import planar.process;

namespace {

namespace proc = planar::process;

// @brief A string that appears NOWHERE else in `src/`, so its presence in
// a witness file or a captured stdout proves the stub really executed.
// Verified with `grep -r` at authoring time; keep it nonsense.
constexpr std::string_view k_sentinel = "zqPROVENANCE-7f3a-plan996-6272-xqz";

// @brief A scratch directory removed when the test leaves scope.
class scratch_dir {
public:
  scratch_dir() {
    auto const base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      auto            candidate = base / std::format("planar-process-t-{}-{}", ::getpid(), attempt);
      std::error_code ec;
      if (std::filesystem::create_directory(candidate, ec)) {
        _path = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("could not create a scratch directory");
  }
  scratch_dir(const scratch_dir&)                    = delete;
  auto operator=(const scratch_dir&) -> scratch_dir& = delete;
  scratch_dir(scratch_dir&&)                         = delete;
  auto operator=(scratch_dir&&) -> scratch_dir&      = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(_path, ec);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return _path;
  }

private:
  std::filesystem::path _path;
};

// @brief Write `body` to `path` and make it executable.
void write_script(const std::filesystem::path& path, std::string_view body) {
  {
    std::ofstream out{path};
    out << body;
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec);
}

// @brief Read a whole file, or an empty string when it is not there.
auto slurp(const std::filesystem::path& path) -> std::string {
  std::ifstream in{path};
  if (!in) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// @brief An `env_lookup` over an explicit map. Nothing here reads the real
// process environment.
auto map_env(std::map<std::string, std::string, std::less<>> vars) -> proc::env_lookup {
  return [vars = std::move(vars)](std::string_view key) -> std::optional<std::string> {
    auto const hit = vars.find(key);
    if (hit == vars.end()) {
      return std::nullopt;
    }
    return hit->second;
  };
}

} // namespace

TEST_CASE("run_inherited executes the program and its argv arrives intact", "[lib][process][spawn]") {
  scratch_dir const scratch;
  auto const        witness = scratch.path() / "argv.txt";
  auto const        stub    = scratch.path() / "stub.sh";

  // WITNESS 1 (argv) and WITNESS 2 (the sentinel) are recorded by the stub
  // itself. Nothing in the seam can produce either without running it.
  write_script(stub, std::format("#!/bin/sh\n"
                                 "printf '%s\\n' '{}' > '{}'\n"
                                 "for a in \"$@\"; do printf '%s\\n' \"$a\" >> '{}'; done\n"
                                 "exit 0\n",
                                 k_sentinel, witness.string(), witness.string()));

  std::vector<std::string> const argv{stub.string(), "run", "/wf/path.lua", "--phase", "closeout"};

  auto const code = proc::run_inherited(map_env({}), argv);

  REQUIRE(code.has_value());
  CHECK(*code == 0);

  auto const recorded = slurp(witness);
  // Non-emptiness asserted FIRST: a witness file that silently stayed empty
  // would let every `contains` assertion below pass vacuously.
  REQUIRE_FALSE(recorded.empty());
  CHECK(recorded.contains(k_sentinel));
  CHECK(recorded == std::format("{}\nrun\n/wf/path.lua\n--phase\ncloseout\n", k_sentinel));
}

TEST_CASE("run_inherited propagates a non-zero exit status exactly", "[lib][process][spawn]") {
  scratch_dir const scratch;
  auto const        stub = scratch.path() / "stub.sh";
  // WITNESS 3 (exit code). 42 is this test's own choice, not read from any
  // constant in the code under test.
  write_script(stub, "#!/bin/sh\nexit 42\n");

  std::vector<std::string> const argv{stub.string()};
  auto const                     code = proc::run_inherited(map_env({}), argv);

  REQUIRE(code.has_value());
  CHECK(*code == 42);
}

TEST_CASE("run_inherited reports signal death as 1", "[lib][process][spawn]") {
  scratch_dir const scratch;
  auto const        stub = scratch.path() / "stub.sh";
  write_script(stub, "#!/bin/sh\nkill -TERM $$\n");

  std::vector<std::string> const argv{stub.string()};
  auto const                     code = proc::run_inherited(map_env({}), argv);

  // Not `std::nullopt`: the program DID run. The oracle folds every
  // non-`.exited` termination to 1, and the distinction from nullopt is the
  // whole contract — nullopt means "never ran".
  REQUIRE(code.has_value());
  CHECK(*code == 1);
}

TEST_CASE("run_inherited passes every argument through untouched", "[lib][process][spawn]") {
  scratch_dir const scratch;
  auto const        witness = scratch.path() / "argv.txt";
  auto const        stub    = scratch.path() / "stub.sh";
  write_script(stub, std::format("#!/bin/sh\n"
                                 "printf '%s\\n' '{}' > '{}'\n"
                                 "for a in \"$@\"; do printf '[%s]\\n' \"$a\" >> '{}'; done\n",
                                 k_sentinel, witness.string(), witness.string()));

  // A space, a single quote and an empty argument: `run_inherited` execs
  // directly rather than through a shell, so none of these need quoting and
  // none may be split, dropped or merged.
  std::vector<std::string> const argv{stub.string(), "two words", "it's", "", "--flag=a b"};

  auto const code = proc::run_inherited(map_env({}), argv);
  REQUIRE(code.has_value());
  CHECK(*code == 0);

  auto const recorded = slurp(witness);
  REQUIRE_FALSE(recorded.empty());
  CHECK(recorded == std::format("{}\n[two words]\n[it's]\n[]\n[--flag=a b]\n", k_sentinel));
}

TEST_CASE("run_inherited reports an unresolvable program as no-run, distinctly from any exit code", "[lib][process][spawn]") {
  scratch_dir const scratch;

  // Absolute path that does not exist.
  std::vector<std::string> const absent{(scratch.path() / "nope.sh").string()};
  CHECK_FALSE(proc::run_inherited(map_env({}), absent).has_value());

  // Bare name with an EMPTY PATH: nothing to search.
  std::vector<std::string> const bare{"planar-no-such-program-6272"};
  CHECK_FALSE(proc::run_inherited(map_env({{"PATH", ""}}), bare).has_value());

  // The ABSENCE assertions above can each pass for the wrong reason (a seam
  // that never runs anything satisfies all of them), so the PRESENT case is
  // asserted here too: the same bare name resolves and runs once PATH names
  // the directory holding it.
  auto const stub = scratch.path() / "planar-no-such-program-6272";
  write_script(stub, "#!/bin/sh\nexit 7\n");
  auto const found = proc::run_inherited(map_env({{"PATH", scratch.path().string()}}), bare);
  REQUIRE(found.has_value());
  CHECK(*found == 7);
}

TEST_CASE("run_inherited refuses an empty argv without forking", "[lib][process][spawn]") {
  std::vector<std::string> const empty;
  CHECK_FALSE(proc::run_inherited(map_env({}), empty).has_value());
}

TEST_CASE("resolve_program searches PATH in order and requires the executable bit", "[lib][process]") {
  scratch_dir const scratch;
  auto const        first  = scratch.path() / "first";
  auto const        second = scratch.path() / "second";
  std::filesystem::create_directory(first);
  std::filesystem::create_directory(second);

  // Present in BOTH, executable only in the second: the first directory
  // must be skipped rather than matched-and-refused.
  {
    std::ofstream out{first / "tool"};
    out << "not executable";
  }
  write_script(second / "tool", "#!/bin/sh\nexit 0\n");

  auto const env      = map_env({{"PATH", std::format("{}:{}", first.string(), second.string())}});
  auto const resolved = proc::resolve_program(env, "tool");
  REQUIRE(resolved.has_value());
  CHECK(*resolved == (second / "tool").string());

  // A name containing '/' is used as-is and never searched.
  CHECK(proc::resolve_program(env, "tool/../tool") == std::nullopt);
  CHECK_FALSE(proc::resolve_program(env, "").has_value());
  // No PATH at all is "cannot resolve", not a crash.
  CHECK_FALSE(proc::resolve_program(map_env({}), "tool").has_value());
}

TEST_CASE("capture runs a REAL subprocess and separates its three outcomes", "[lib][process][spawn]") {
  // Outcome 1: the program does not exist at all. `posix_spawnp` reports
  // ENOENT directly, so this is `spawned == false` — NOT an exit 127, which
  // is what a shell-based runner would give and which a real program can
  // also return.
  constexpr std::array<std::string_view, 0> k_none{};
  auto const                                missing = proc::capture("planar-no-such-program-6272", k_none);
  CHECK_FALSE(missing.spawned);

  // Outcome 2: the program exists and exits NON-ZERO. `/usr/bin/false` is
  // on every platform this builds for.
  auto const failed = proc::capture("false", k_none);
  CHECK(failed.spawned);
  CHECK(failed.exit_code != 0);

  // Outcome 3: the program exists and succeeds, with output. The pair
  // (missing, failed) is the load-bearing one: both are "no token", and the
  // caller renders them as DIFFERENT refusals.
  constexpr std::array<std::string_view, 1> k_echo_args{"hello"};
  auto const                                ok = proc::capture("echo", k_echo_args);
  CHECK(ok.spawned);
  CHECK(ok.exit_code == 0);
  CHECK(ok.output == "hello\n");
}

TEST_CASE("capture returns the child's stdout verbatim, untrimmed", "[lib][process][spawn]") {
  scratch_dir const scratch;
  auto const        stub = scratch.path() / "stub.sh";
  // The sentinel is WITNESS 2 again, this time arriving through the pipe
  // rather than a file: a `capture` that never spawned cannot produce it.
  // The surrounding whitespace pins "untrimmed" — trimming belongs to the
  // caller (`ext_adapter_factory` does it, deliberately, with its own
  // four-character set).
  write_script(stub, std::format("#!/bin/sh\nprintf '  {}  \\n\\n'\nexit 0\n", k_sentinel));

  constexpr std::array<std::string_view, 0> k_none{};
  auto const                                got = proc::capture(stub.string(), k_none);

  REQUIRE(got.spawned);
  CHECK(got.exit_code == 0);
  REQUIRE_FALSE(got.output.empty());
  CHECK(got.output == std::format("  {}  \n\n", k_sentinel));
}

TEST_CASE("capture passes its arguments through and discards the child's stderr", "[lib][process][spawn]") {
  scratch_dir const scratch;
  auto const        witness = scratch.path() / "argv.txt";
  auto const        stub    = scratch.path() / "stub.sh";
  write_script(stub, std::format("#!/bin/sh\n"
                                 "for a in \"$@\"; do printf '%s\\n' \"$a\" >> '{}'; done\n"
                                 "printf 'THIS-MUST-NOT-BE-CAPTURED\\n' >&2\n"
                                 "printf '{}\\n'\n",
                                 witness.string(), k_sentinel));

  constexpr std::array<std::string_view, 2> k_args{"auth", "token"};
  auto const                                got = proc::capture(stub.string(), k_args);

  REQUIRE(got.spawned);
  auto const recorded = slurp(witness);
  REQUIRE_FALSE(recorded.empty());
  CHECK(recorded == "auth\ntoken\n");
  // stderr went to /dev/null, so it is absent from the capture — the reason
  // a failing `gh` never reaches the operator's terminal.
  CHECK_FALSE(got.output.contains("THIS-MUST-NOT-BE-CAPTURED"));
  CHECK(got.output == std::format("{}\n", k_sentinel));
}
