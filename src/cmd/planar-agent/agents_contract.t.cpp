// agents_contract.t.cpp: the frontmatter contract of the authored agent role
// files (plan 1104, task 7209; tech spec: agent files are named
// `planar-<role>.md` and carry `name` and `description` at the top level and
// `kind` and `slug` under a `planar` map).
//
// The role files are installed one to a vendor directory, so each is named
// `planar-<role>` to keep it from colliding with another tool's agent of the
// same role. This file holds the sources to that contract, holds the four
// doctrine documents to staying out of it, checks that no authored text still
// dispatches an agent by its bare role name, and checks that the bare role
// VALUE (`--role coder`) is still what `planar-agent queue run` accepts.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {

namespace parity = planar::cmd::parity;

/// @brief The fifteen role files, by stem.
constexpr std::array k_agent_stems{
    "planar-coder",       "planar-reviewer", "planar-orchestrator",     "planar-test-coder",   "planar-janitor",
    "planar-research",    "planar-planner",  "planar-spec-reviewer",    "planar-ingestor",     "planar-importer",
    "planar-synthesizer", "planar-ext-sync", "planar-feedback-triager", "planar-introspector", "planar-sync-reconciler",
};

/// @brief The doctrine documents: shared text, not agents, and not renamed.
constexpr std::array k_doctrine{"methodology.md", "doctrine.md", "models.md", "cross-scope-writes.md"};

auto repo(std::string_view rel) -> std::filesystem::path {
  return std::filesystem::path{PLANAR_REPO_ROOT} / rel;
}

auto read_text(std::filesystem::path const& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

/// @brief The frontmatter of one file: its top-level keys and the `planar` map.
struct front {
  bool                               has_block = false;
  std::map<std::string, std::string> top; ///< Top-level key to value.
  bool                               has_planar = false;
  std::map<std::string, std::string> planar; ///< Keys of the `planar` map.
};

auto trim(std::string_view s) -> std::string {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return std::string{s};
}

auto parse_front(std::string_view text) -> front {
  front out;
  if (!text.starts_with("---\n")) {
    return out;
  }
  auto const close = text.find("\n---", 4);
  if (close == std::string_view::npos) {
    return out;
  }
  out.has_block = true;
  std::istringstream lines{std::string{text.substr(4, close - 4)}};
  bool               in_planar = false;
  for (std::string line; std::getline(lines, line);) {
    if (line.empty()) {
      continue;
    }
    bool const indented = line.front() == ' ' || line.front() == '\t';
    auto const colon    = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    auto const key = trim(std::string_view{line}.substr(0, colon));
    auto const val = trim(std::string_view{line}.substr(colon + 1));
    if (indented) {
      if (in_planar) {
        out.planar[key] = val;
      }
      continue;
    }
    in_planar = false;
    if (key == "planar") {
      out.has_planar = true;
      in_planar      = true;
      continue;
    }
    out.top[key] = val;
  }
  return out;
}

/// @brief Every way one agent role file breaks the contract, each naming the file.
/// @param file The file name, e.g. `planar-coder.md`.
auto agent_problems(std::string const& file, std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> problems;
  auto const               fm = parse_front(text);
  if (!fm.has_block) {
    problems.push_back(std::format("{}: no frontmatter block", file));
    return problems;
  }
  for (auto const* moved : {"kind", "slug"}) {
    if (fm.top.contains(moved)) {
      problems.push_back(std::format("{}: top-level `{}` must move under the `planar` map", file, moved));
    }
  }
  for (auto const* gone : {"model", "codex"}) {
    if (fm.top.contains(gone)) {
      problems.push_back(std::format("{}: top-level `{}` is not allowed", file, gone));
    }
  }
  if (!fm.has_planar) {
    problems.push_back(std::format("{}: no `planar` map", file));
    return problems;
  }
  auto const stem = file.ends_with(".md") ? file.substr(0, file.size() - 3) : file;
  if (!file.starts_with("planar-")) {
    problems.push_back(std::format("{}: file name must start with `planar-`", file));
  }
  auto const name = fm.top.contains("name") ? fm.top.at("name") : std::string{};
  auto const slug = fm.planar.contains("slug") ? fm.planar.at("slug") : std::string{};
  auto const kind = fm.planar.contains("kind") ? fm.planar.at("kind") : std::string{};
  if (kind != "agent") {
    problems.push_back(std::format("{}: planar.kind is `{}`, expected `agent`", file, kind));
  }
  if (name != slug) {
    problems.push_back(std::format("{}: planar.slug `{}` differs from name `{}`", file, slug, name));
  }
  if (name != stem) {
    problems.push_back(std::format("{}: name `{}` differs from file stem `{}`", file, name, stem));
  }
  if (!fm.top.contains("description") || fm.top.at("description").empty()) {
    problems.push_back(std::format("{}: no top-level `description`", file));
  }
  return problems;
}

auto joined(std::vector<std::string> const& v) -> std::string {
  std::string out;
  for (auto const& s : v) {
    out += s + "\n";
  }
  return out;
}

auto has_problem(std::vector<std::string> const& problems, std::string_view needle) -> bool {
  return std::ranges::any_of(problems, [&](std::string const& p) { return p.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("agents/ holds exactly fifteen planar-prefixed role files that follow the frontmatter contract",
          "[cmd][agent][agents-contract]") {
  std::vector<std::string> agents;
  for (auto const& entry : std::filesystem::directory_iterator(repo("agents"))) {
    if (!entry.is_regular_file() || entry.path().extension() != ".md") {
      continue;
    }
    auto const file = entry.path().filename().string();
    auto const fm   = parse_front(read_text(entry.path()));
    if (fm.has_planar && fm.planar.contains("kind") && fm.planar.at("kind") == "agent") {
      agents.push_back(file);
      auto const problems = agent_problems(file, read_text(entry.path()));
      INFO(joined(problems));
      CHECK(problems.empty());
    }
  }
  std::ranges::sort(agents);
  std::vector<std::string> expected;
  for (auto const* stem : k_agent_stems) {
    expected.push_back(std::string{stem} + ".md");
  }
  std::ranges::sort(expected);
  CHECK(agents.size() == 15);
  CHECK(agents == expected);
}

TEST_CASE("the four doctrine documents keep their names and carry no planar map and no name", "[cmd][agent][agents-contract]") {
  for (auto const* file : k_doctrine) {
    DYNAMIC_SECTION(file) {
      REQUIRE(std::filesystem::exists(repo(std::string{"agents/"} + file)));
      auto const fm = parse_front(read_text(repo(std::string{"agents/"} + file)));
      REQUIRE(fm.has_block);
      CHECK_FALSE(fm.has_planar);
      CHECK_FALSE(fm.top.contains("name"));
    }
  }
  // No other file is in agents/: every .md is a role file or one of the four.
  std::size_t count = 0;
  for (auto const& entry : std::filesystem::directory_iterator(repo("agents"))) {
    count += entry.is_regular_file() && entry.path().extension() == ".md" ? 1 : 0;
  }
  CHECK(count == k_agent_stems.size() + k_doctrine.size());
}

TEST_CASE("a planar.slug that differs from the name fails and names the file and both values", "[cmd][agent][agents-contract]") {
  auto const problems =
      agent_problems("planar-coder.md", "---\nname: planar-coder\ndescription: d\nplanar:\n  kind: agent\n  slug: coder\n---\n");
  INFO(joined(problems));
  REQUIRE_FALSE(problems.empty());
  CHECK(has_problem(problems, "planar-coder.md: planar.slug `coder` differs from name `planar-coder`"));
}

TEST_CASE("a top-level kind with no planar map fails and names the key to move", "[cmd][agent][agents-contract]") {
  auto const problems =
      agent_problems("planar-coder.md", "---\nname: planar-coder\ndescription: d\nkind: agent\nslug: planar-coder\n---\n");
  INFO(joined(problems));
  CHECK(has_problem(problems, "planar-coder.md: top-level `kind` must move under the `planar` map"));
  CHECK(has_problem(problems, "planar-coder.md: top-level `slug` must move under the `planar` map"));
  CHECK(has_problem(problems, "no `planar` map"));
}

TEST_CASE("a file with a top-level model or a bare file name fails", "[cmd][agent][agents-contract]") {
  auto const model =
      agent_problems("planar-coder.md",
                     "---\nname: planar-coder\ndescription: d\nmodel: opus\nplanar:\n  kind: agent\n  slug: planar-coder\n---\n");
  CHECK(has_problem(model, "planar-coder.md: top-level `model` is not allowed"));
  auto const bare = agent_problems("coder.md", "---\nname: coder\ndescription: d\nplanar:\n  kind: agent\n  slug: coder\n---\n");
  CHECK(has_problem(bare, "coder.md: file name must start with `planar-`"));
}

TEST_CASE("no authored text dispatches an agent by its bare role name", "[cmd][agent][agents-contract]") {
  // `--role coder`, `routing_candidate_bindings.role` and the models.md tier rows
  // stay bare: they are role values, not agent names, and are not matched here.
  std::string const roles =
      "coder|reviewer|orchestrator|test-coder|janitor|research|planner|spec-reviewer|ingestor|importer|synthesizer|ext-sync|"
      "feedback-triager|introspector|sync-reconciler";
  std::regex const dispatch{"(subagent_type|agent_type|agent)[ \\t]*[:=][ \\t]*[`\"']?(" + roles + ")\\b|--agent[ =](" + roles +
                            ")\\b|subagent type `(" + roles + ")`"};

  std::vector<std::filesystem::path> files;
  for (auto const* dir : {"agents", "evals", "docs"}) {
    for (auto const& entry : std::filesystem::recursive_directory_iterator(repo(dir))) {
      auto const ext = entry.path().extension().string();
      if (entry.is_regular_file() && (ext == ".md" || ext == ".json" || ext == ".py" || ext == ".sh")) {
        files.push_back(entry.path());
      }
    }
  }
  files.push_back(repo("CLAUDE.md"));

  std::vector<std::string> hits;
  for (auto const& path : files) {
    std::istringstream in(read_text(path));
    std::size_t        n = 0;
    for (std::string line; std::getline(in, line);) {
      ++n;
      if (line.find("agent") != std::string::npos && std::regex_search(line, dispatch)) {
        hits.push_back(std::format("{}:{}: {}", path.string(), n, line));
      }
    }
  }
  INFO(joined(hits));
  CHECK(hits.empty());
}

TEST_CASE("queue run still accepts the bare role value", "[cmd][agent][agents-contract][queue]") {
  auto arena = parity::make_arena("agents-contract-queue");
  planar::cmd::qfix::head_store(arena.cpp_root / "planar.db");
  std::vector<std::string> const args{"queue", "run", "--vendor", "claude", "--role", "coder", "--", "true"};
  auto const got = parity::run_pinned(std::filesystem::path{PLANAR_CPP_BIN}, args, arena.cpp_root, "bare-role");
  INFO("stdout:\n" << got.out << "stderr:\n" << got.err);
  CHECK(got.code == 0);
}
