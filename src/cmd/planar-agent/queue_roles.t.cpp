// queue_roles.t.cpp: the rule's short form in the role files and skill sources
// (plan 1080, task hq-role-files; tech spec 647 § Codex is reached through
// role files; test spec 649 "every vendor's role files carry the short rule").
//
// Scriptorium does not render `agents/methodology.md` for Codex, so each role
// file and each skill source carries the short form itself. These cases read
// the authored sources from the repository and fail when one of them loses an
// element the short form needs, or when the coder's own instructions go back
// to a direct build or test command.
//
// The command line is not typed here a second time: it is read out of the
// authored rule file, so the short form cannot drift from the rule.

#include <catch2/catch_test_macros.hpp>

import std;

namespace {

/// @brief Every authored source that must carry the short form. The janitor has
/// no skill source.
constexpr std::array k_sources{
    "agents/coder.md",
    "agents/test-coder.md",
    "agents/reviewer.md",
    "agents/janitor.md",
    "agents/orchestrator.md",
    "skills/src/pl-coder.md",
    "skills/src/pl-test-coder.md",
    "skills/src/pl-reviewer.md",
    "skills/src/pl-orchestrator.md",
};

auto read_file(std::filesystem::path const& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

auto repo(std::string_view rel) -> std::filesystem::path {
  return std::filesystem::path{PLANAR_REPO_ROOT} / rel;
}

auto contains(std::string_view text, std::string_view needle) -> bool {
  return text.find(needle) != std::string_view::npos;
}

/// @brief The submit command line, taken from the authored rule file.
auto rule_command_line() -> std::string {
  auto const         rule = read_file(repo("src/engine/hostqueue/queue-rule.md"));
  std::istringstream lines(rule);
  std::string        line;
  while (std::getline(lines, line)) {
    if (line.starts_with("planar-agent queue run --detach")) {
      return line;
    }
  }
  FAIL("the rule file has no detached submit line");
  return {};
}

/// @brief Text with each run of white space collapsed to one space, so a check
/// does not depend on where a paragraph wraps.
auto flat(std::string_view text) -> std::string {
  std::string out;
  bool        space = false;
  for (char c : text) {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      space = true;
      continue;
    }
    if (space && !out.empty()) {
      out.push_back(' ');
    }
    space = false;
    out.push_back(c);
  }
  return out;
}

/// @brief Does a fragment of a line start with a build or test command?
auto starts_with_build_command(std::string_view s) -> bool {
  for (auto const* word : {"make", "cmake", "ctest", "ninja", "cargo build", "cargo test", "go build", "go test", "npm test",
                           "pytest", "tox", "gradle"}) {
    std::string_view w{word};
    if (s.starts_with(w) && (s.size() == w.size() || s[w.size()] == ' ' || s[w.size()] == '`')) {
      return true;
    }
  }
  return false;
}

/// @brief Lines of code blocks and inline code spans that begin with a build or
/// test command that is not given to the queue. A line carrying the
/// `queue-lint-ignore` marker is exempt.
auto direct_build_instructions(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> found;
  bool                     fenced = false;
  std::istringstream       lines{std::string{text}};
  std::string              line;
  while (std::getline(lines, line)) {
    if (contains(line, "queue-lint-ignore")) {
      continue;
    }
    auto const first = line.find_first_not_of(" \t");
    auto const body  = first == std::string::npos ? std::string_view{} : std::string_view{line}.substr(first);
    if (body.starts_with("```")) {
      fenced = !fenced;
      continue;
    }
    if (fenced) {
      if (starts_with_build_command(body)) {
        found.push_back(line);
      }
      continue;
    }
    for (std::size_t open = line.find('`'); open != std::string::npos;) {
      auto const close = line.find('`', open + 1);
      if (close == std::string::npos) {
        break;
      }
      if (starts_with_build_command(std::string_view{line}.substr(open + 1, close - open))) {
        found.push_back(line);
      }
      open = line.find('`', close + 1);
    }
  }
  return found;
}

} // namespace

TEST_CASE("every role file and skill source carries the short form of the queue rule", "[cmd][agent][queue][roles]") {
  auto const command = rule_command_line();
  REQUIRE(contains(command, "--vendor <vendor> --role <role> -- <command>"));

  for (auto const* rel : k_sources) {
    DYNAMIC_SECTION(rel) {
      auto const text = flat(read_file(repo(rel)));

      // The command form, exactly as the rule gives it.
      CHECK(contains(text, flat(command)));
      // The polling procedure and its interval.
      CHECK(contains(text, "planar-agent queue status <seq>"));
      CHECK(contains(text, "every 30 seconds"));
      // Exit 125 means stop, and the command is not run directly.
      CHECK(contains(text, "exit 125"));
      CHECK(contains(text, "must not be run directly"));
      // The no-queue check and what a failure of it means.
      CHECK(contains(text, "planar-agent queue rule >/dev/null"));
      CHECK(contains(text, "run the command directly and tell the operator Planar needs upgrading"));
      // The pointer to the full text.
      CHECK(contains(text, "`planar-agent queue rule` prints the full rule"));
    }
  }
}

TEST_CASE("a role file or skill source gives no build or test command to run directly", "[cmd][agent][queue][roles]") {
  for (auto const* rel : k_sources) {
    DYNAMIC_SECTION(rel) {
      auto const found = direct_build_instructions(read_file(repo(rel)));
      for (auto const& line : found) {
        INFO("direct build or test command: " << line);
      }
      CHECK(found.empty());
    }
  }
}

TEST_CASE("the coder's gate instructions use the queued form", "[cmd][agent][queue][roles]") {
  auto const text = flat(read_file(repo("agents/coder.md")));
  // Long profile commands are submitted detached, short ones run in the
  // foreground, and neither names a make target: the files are installed for
  // every repository and defer to its confirmed validation profile.
  CHECK(contains(text, "planar-agent queue run --detach --vendor <vendor> --role coder -- <profile command>"));
  CHECK(contains(text, "planar-agent queue run --vendor <vendor> --role coder -- <profile command>"));
}

TEST_CASE("no role file or skill source names a make target in a queue line", "[cmd][agent][queue][roles]") {
  for (auto const* rel : k_sources) {
    DYNAMIC_SECTION(rel) {
      std::istringstream in(read_file(repo(rel)));
      for (std::string line; std::getline(in, line);) {
        auto const at = line.find("planar-agent queue run");
        if (at == std::string::npos) {
          continue;
        }
        INFO("queue line names a make target: " << line);
        CHECK(line.find("make ", at) == std::string::npos);
      }
    }
  }
}

TEST_CASE("the direct-instruction check discriminates", "[cmd][agent][queue][roles]") {
  CHECK(direct_build_instructions("Run `make test` first.\n").size() == 1);
  CHECK(direct_build_instructions("```\nctest --test-dir build\n```\n").size() == 1);
  CHECK(direct_build_instructions("  ```sh\n  cmake --build build/debug\n  ```\n").size() == 1);
  CHECK(direct_build_instructions("Run `planar-agent queue run -- make test` first.\n").empty());
  CHECK(direct_build_instructions("a ~9-minute ctest run\n").empty());
  CHECK(direct_build_instructions("`make cpp-lint` names the gate. queue-lint-ignore\n").empty());
}
