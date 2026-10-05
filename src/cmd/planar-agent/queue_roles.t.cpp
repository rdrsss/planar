// queue_roles.t.cpp: the rule's citation in the role files (plan 1080, task
// hq-role-files; tech spec 647 § Codex is reached through role files; test spec
// 649 "every vendor's role files carry the short rule"; plan 1104 task 7214).
//
// The agent files are the only role text. Each role file carries one sentence
// citing the host build queue rule in `agents/methodology.md`. These cases read
// the authored sources from the repository and fail when one of them loses that
// citation, or when the coder's own instructions stop using the queued form.
// That no source tells an agent to run a build or test command directly is not
// checked here: `surface_lint` owns that rule (surface-queue-command,
// src/tools/surface_lint/main.cpp), over all of `agents/`.

#include <catch2/catch_test_macros.hpp>

import std;

namespace {

/// @brief Every role file, which cites the methodology's queue rule.
constexpr std::array k_role_files{
    "agents/planar-coder.md",   "agents/planar-test-coder.md",   "agents/planar-reviewer.md",
    "agents/planar-janitor.md", "agents/planar-orchestrator.md",
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

} // namespace

TEST_CASE("every role file cites the methodology's host build queue rule", "[cmd][agent][queue][roles]") {
  for (auto const* rel : k_role_files) {
    DYNAMIC_SECTION(rel) {
      auto const text = flat(read_file(repo(rel)));
      CHECK(contains(text, "host build queue rule in `methodology.md` in the Planar agents directory"));
    }
  }
}

TEST_CASE("the coder's gate instructions use the queued form", "[cmd][agent][queue][roles]") {
  auto const text = flat(read_file(repo("agents/planar-coder.md")));
  // Long profile commands are submitted detached, short ones run in the
  // foreground, and neither names a make target: the files are installed for
  // every repository and defer to its confirmed validation profile.
  CHECK(contains(text, "planar-agent queue run --detach --vendor <vendor> --role coder -- <profile command>"));
  CHECK(contains(text, "planar-agent queue run --vendor <vendor> --role coder -- <profile command>"));
}

TEST_CASE("no role file names a make target in a queue line", "[cmd][agent][queue][roles]") {
  for (auto const rel : k_role_files) {
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
