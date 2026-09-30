// @file guard.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.guard` (plan 1080, task
// hq-command-guard). Covers the test-spec scenarios "Error -- a model
// launcher is refused" and "Edge -- a program whose name only contains a
// listed word is allowed" at the level of the decision; the black-box cases in
// src/cmd/planar-agent/queue_run_checks.t.cpp cover the same scenarios through
// the built binary, with the store read back.
//
// The list is spelled out here rather than read from the module, so a change
// to it has to change the test too.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.hostqueue;

namespace {

namespace hq = planar::engine::hostqueue;

constexpr std::array<std::string_view, 7> k_launchers{"claude", "codex",    "gemini",      "copilot",
                                                      "aider",  "opencode", "cursor-agent"};

/// @brief The launcher `argv` is refused for, or an empty string when allowed.
auto refused_as(std::vector<std::string> argv) -> std::string {
  auto const verdict = hq::check_command(argv);
  return verdict ? std::string{} : verdict.error().program;
}

} // namespace

TEST_CASE("the launcher list is the tech spec's, in order", "[engine][hostqueue][hq-command-guard]") {
  auto const list = hq::model_launchers();
  REQUIRE(list.size() == k_launchers.size());
  for (std::size_t i = 0; i < list.size(); ++i) {
    CHECK(list[i] == k_launchers[i]);
  }
}

TEST_CASE("every launcher is refused as a bare name, with a directory prefix, and with arguments",
          "[engine][hostqueue][hq-command-guard]") {
  for (auto const name : k_launchers) {
    INFO(name);
    auto const n = std::string{name};
    CHECK(refused_as({n}) == n);
    CHECK(refused_as({n, "-p", "hello"}) == n);
    CHECK(refused_as({"/usr/local/bin/" + n}) == n);
    CHECK(refused_as({"./" + n}) == n);
    CHECK(refused_as({"../bin/" + n}) == n);
    CHECK(refused_as({"/opt/x/" + n + "/"}) == n); // Trailing slash: still that name.
  }
}

TEST_CASE("a name that only contains a listed word, or differs in case, is allowed", "[engine][hostqueue][hq-command-guard]") {
  for (auto const* name : {"codex-lint-report", "claude_fixture", "claudette", "myclaude", "aider2", "Claude", "CODEX", "cursor",
                           "cursor-agents", "opencode.sh", "/usr/bin/claude-code-helper", "/opt/claude/bin/make"}) {
    INFO(name);
    CHECK(refused_as({name}) == "");
  }
  // The directory is never the program: only the last component counts.
  CHECK(refused_as({"/home/u/claude/bin/make", "test"}) == "");
  // A listed word as an ARGUMENT is not the program.
  CHECK(refused_as({"make", "claude"}) == "");
  CHECK(refused_as({"echo", "codex"}) == "");
}

TEST_CASE("leading assignments and a leading env are skipped before the program is chosen",
          "[engine][hostqueue][hq-command-guard]") {
  CHECK(refused_as({"VAR=value", "claude"}) == "claude");
  CHECK(refused_as({"A=1", "B=2", "codex", "exec"}) == "codex");
  CHECK(refused_as({"env", "claude"}) == "claude");
  CHECK(refused_as({"env", "VAR=value", "claude"}) == "claude");
  CHECK(refused_as({"/usr/bin/env", "gemini"}) == "gemini");
  CHECK(refused_as({"env", "-i", "copilot"}) == "copilot");
  CHECK(refused_as({"env", "-i", "A=1", "B=2", "aider"}) == "aider");
  CHECK(refused_as({"env", "-u", "NAME", "opencode"}) == "opencode");
  CHECK(refused_as({"env", "-uNAME", "cursor-agent"}) == "cursor-agent");
  CHECK(refused_as({"env", "--unset=NAME", "claude"}) == "claude");
  CHECK(refused_as({"env", "--unset", "NAME", "claude"}) == "claude");
  CHECK(refused_as({"env", "-C", "/tmp", "claude"}) == "claude");
  CHECK(refused_as({"env", "--chdir=/tmp", "claude"}) == "claude");
  CHECK(refused_as({"env", "-i", "-u", "A", "-u", "B", "claude"}) == "claude");
  CHECK(refused_as({"env", "-iu", "NAME", "claude"}) == "claude");
  CHECK(refused_as({"env", "-0", "-v", "claude"}) == "claude");
  CHECK(refused_as({"env", "--", "claude"}) == "claude");
  CHECK(refused_as({"env", "-i", "--", "A=1", "claude"}) == "claude");
  CHECK(refused_as({"env", "env", "claude"}) == "claude");
  CHECK(refused_as({"env", "A=1", "env", "-i", "B=2", "claude"}) == "claude");
  CHECK(refused_as({"A=1", "env", "claude"}) == "claude");
}

TEST_CASE("env -S is split into words and its first word is the program", "[engine][hostqueue][hq-command-guard]") {
  CHECK(refused_as({"env", "-S", "claude -p hi"}) == "claude");
  CHECK(refused_as({"env", "-Sclaude -p hi"}) == "claude");
  CHECK(refused_as({"env", "--split-string=claude -p hi"}) == "claude");
  CHECK(refused_as({"env", "--split-string", "codex exec"}) == "codex");
  CHECK(refused_as({"env", "-S", "A=1 B=2 gemini"}) == "gemini");
  CHECK(refused_as({"env", "-S", "-i claude"}) == "claude");
  CHECK(refused_as({"env", "-S", "'claude' -p hi"}) == "claude");
  CHECK(refused_as({"env", "-S", "\"/usr/bin/copilot\" x"}) == "copilot");
  CHECK(refused_as({"env", "-iS", "claude"}) == "claude");
  CHECK(refused_as({"env", "-S", "env claude"}) == "claude");
  CHECK(refused_as({"env", "-S", "make test"}) == "");
  CHECK(refused_as({"env", "-S", ""}) == "");
}

TEST_CASE("env that runs nothing, or runs something else, is allowed", "[engine][hostqueue][hq-command-guard]") {
  CHECK(refused_as({}) == "");
  CHECK(refused_as({"env"}) == "");
  CHECK(refused_as({"env", "-i"}) == "");
  CHECK(refused_as({"env", "A=1", "B=2"}) == "");
  CHECK(refused_as({"env", "-u"}) == "");
  CHECK(refused_as({"env", "--"}) == "");
  CHECK(refused_as({"env", "make", "claude"}) == "");
  CHECK(refused_as({"env", "-u", "claude", "make"}) == ""); // The value of -u is a name, not the program.
  CHECK(refused_as({"env", "-C", "claude", "make"}) == "");
  CHECK(refused_as({"env", "A=claude", "make"}) == "");
  CHECK(refused_as({"envx", "claude"}) == ""); // Not env: `envx` is the program.
  CHECK(refused_as({"env-claude"}) == "");
}

TEST_CASE("an assignment is a shell identifier before env and any name after it", "[engine][hostqueue][hq-command-guard]") {
  // Before env, `1A=x` and `A-B=x` are not assignments: they are a program
  // name, which is allowed here (and will not resolve).
  CHECK(refused_as({"1A=x", "claude"}) == "");
  CHECK(refused_as({"A-B=x", "claude"}) == "");
  CHECK(refused_as({"=x", "claude"}) == "");
  // After env, any non-empty name before `=` is an assignment.
  CHECK(refused_as({"env", "A-B=x", "claude"}) == "claude");
  CHECK(refused_as({"env", "1A=x", "claude"}) == "claude");
}

TEST_CASE("a pathological chain of env words ends", "[engine][hostqueue][hq-command-guard]") {
  std::vector<std::string> argv(10'000, "env");
  argv.emplace_back("claude");
  // Ends rather than looping. The chain is longer than the guard follows, so
  // it makes no promise about the verdict, only that it returns.
  static_cast<void>(hq::check_command(argv));
  SUCCEED();
}
