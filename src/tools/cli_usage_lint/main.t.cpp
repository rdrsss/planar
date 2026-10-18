// @file main.t.cpp
// @brief Standing ctest coverage for `cli_usage_lint`.
//
// The tool shells `<bin> schema`, so each case builds a synthetic repository
// root and a fake `planar-agent` (a shell script that prints a small fixed
// catalog). A case therefore controls both halves of the comparison: the
// flags the catalog allows and the authored text that uses them.
//
// Every "stays silent" assertion has a twin that fires on the same shape, so
// a tool that scanned nothing cannot pass for a tool that found nothing.

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS

import std;

namespace {

namespace fs = std::filesystem;

/// @brief A small `planar-agent schema` catalog: two leaf verbs, one of which
/// takes a trailing command after a bare `--` in the examples below.
constexpr std::string_view k_schema =
    R"({"commands":[)"
    R"({"command":"planar-agent","subcommands":["heartbeat","claim"],"flags":[]},)"
    R"({"command":"planar-agent heartbeat","subcommands":[],"flags":[)"
    R"({"long":"--claim"},{"long":"--status"},{"long":"--ttl"}]},)"
    R"({"command":"planar-agent claim","subcommands":[],"flags":[{"long":"--entity"},{"long":"--json"}]})"
    R"(]})";

/// @brief A scratch repository root holding a fake `planar-agent`. Removed on
/// destruction; named `planar_*` so the arena-sweep listener reaps it too.
struct repo_root {
  fs::path root;

  explicit repo_root(std::string_view tag, std::string_view schema = k_schema) {
    root = fs::temp_directory_path() /
           std::format("planar_cli_usage_lint_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root / "bin");
    auto const script = root / "bin" / "planar-agent";
    {
      std::ofstream out(script, std::ios::binary);
      out << "#!/bin/sh\ncat <<'EOF'\n" << schema << "\nEOF\n";
    }
    fs::permissions(script, fs::perms::owner_all);
  }
  ~repo_root() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  repo_root(repo_root const&)                    = delete;
  auto operator=(repo_root const&) -> repo_root& = delete;

  auto write(std::string_view rel, std::string_view text) const -> void {
    auto const path = root / rel;
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
  }
  [[nodiscard]] auto path(std::string_view rel) const -> std::string {
    return (root / rel).string();
  }
};

struct run_result {
  std::string out;
  int         code = -1;
};

/// @brief Run the tool over `repo`, merging stdout and stderr.
auto run_lint(repo_root const& repo) -> run_result {
  auto const    out_path = repo.root / "lint-output.txt";
  auto const    cmd      = std::format("'{}' '{}' '{}' > '{}' 2>&1", PLANAR_CLI_USAGE_LINT_BIN, repo.root.string(),
                                       (repo.root / "bin" / "planar-agent").string(), out_path.string());
  int const     status   = std::system(cmd.c_str());
  std::ifstream in(out_path, std::ios::binary);
  std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(WIFEXITED(status));
  return {.out = std::move(text), .code = WEXITSTATUS(status)};
}

auto contains(std::string_view text, std::string_view needle) -> bool {
  return text.find(needle) != std::string_view::npos;
}

} // namespace

TEST_CASE("a flag error before a bare -- is still reported", "[cli_usage_lint][cli-usage]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root repo{"before"};

  SECTION("inline span") {
    repo.write("agents/a.md", "# A\n\nRun `planar-agent heartbeat --bogus -- make test` now.\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(contains(r.out, repo.path("agents/a.md") + ":3: `planar-agent heartbeat` has no flag `--bogus`"));
    CHECK(contains(r.out, "cli-usage-lint: 1 violation(s)"));
  }

  SECTION("fenced block") {
    repo.write("agents/a.md", "# A\n\n```\nplanar-agent heartbeat --claim t --bogus -- make test\n```\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(contains(r.out, repo.path("agents/a.md") + ":4: `planar-agent heartbeat` has no flag `--bogus`"));
  }

  SECTION("one line: the flag before is reported, the flag after the bare -- is not") {
    repo.write("agents/a.md", "# A\n\n`planar-agent heartbeat --bogus -- make --other`\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(contains(r.out, "has no flag `--bogus`"));
    CHECK_FALSE(contains(r.out, "--other"));
    CHECK(contains(r.out, "cli-usage-lint: 1 violation(s)"));
  }
}

TEST_CASE("flag checking stops at a bare --", "[cli_usage_lint][cli-usage]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root repo{"after"};

  SECTION("another program's flags after the -- are not this catalog's to check") {
    repo.write("agents/a.md", "# A\n\n`planar-agent heartbeat --claim t -- make --bogus-flag -j8`\n\n"
                              "```\nplanar-agent heartbeat --claim t --status ok -- pytest --bogus-too\n```\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 0);
    CHECK(contains(r.out, "cli-usage-lint: clean"));
  }

  SECTION("the same flag with no -- before it is reported") {
    repo.write("agents/a.md", "# A\n\n`planar-agent heartbeat make --bogus-flag`\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(contains(r.out, "has no flag `--bogus-flag`"));
  }
}

// ---------------------------------------------------------------------------
// Catalog mode (plan 1104, task 7203): the lint also reads each binary's own
// `docs.examples` and validates them like authored prose.
// ---------------------------------------------------------------------------

namespace {

/// @brief The catalog with `examples` as the `docs.examples` of
/// `planar-agent heartbeat`.
/// @param examples The JSON array body, e.g. `"planar-agent heartbeat --claim t"`.
auto schema_with_examples(std::string_view examples) -> std::string {
  return std::format(
      R"({{"commands":[)"
      R"({{"command":"planar-agent","subcommands":["heartbeat","claim"],"flags":[]}},)"
      R"({{"command":"planar-agent heartbeat","subcommands":[],"flags":[{{"long":"--claim"}},{{"long":"--status"}}],)"
      R"("docs":{{"examples":[{}],"exitCodes":[]}}}},)"
      R"({{"command":"planar-agent claim","subcommands":[],"flags":[{{"long":"--entity"}},{{"long":"--json"}}],)"
      R"("docs":{{"examples":["planar-agent claim --entity task:1 --json"],"exitCodes":[]}}}})"
      R"(]}})",
      examples);
}

} // namespace

TEST_CASE("a catalog example with an unknown flag is reported with its command path and the flag",
          "[cli_usage_lint][cli-usage][catalog]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root  repo{"catalog_bogus", schema_with_examples(R"("planar-agent heartbeat --bogus -- make test")")};
  auto const r = run_lint(repo);
  INFO(r.out);
  CHECK(r.code == 1);
  CHECK(contains(r.out, "docs.examples"));
  CHECK(contains(r.out, "`planar-agent heartbeat` has no flag `--bogus`"));
  CHECK(contains(r.out, "2 catalog examples"));
}

TEST_CASE("valid catalog examples are counted and pass", "[cli_usage_lint][cli-usage][catalog]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root  repo{"catalog_clean", schema_with_examples(R"("planar-agent heartbeat --claim t --status ok -- make test")")};
  auto const r = run_lint(repo);
  INFO(r.out);
  CHECK(r.code == 0);
  CHECK(contains(r.out, "cli-usage-lint: clean (0 files"));
  CHECK(contains(r.out, "2 catalog examples"));
}

TEST_CASE("a catalog example that invokes another command is reported", "[cli_usage_lint][cli-usage][catalog]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root  repo{"catalog_other", schema_with_examples(R"("planar-agent claim task:7")")};
  auto const r = run_lint(repo);
  INFO(r.out);
  CHECK(r.code == 1);
  CHECK(contains(r.out, "`planar-agent heartbeat` has an example that invokes `planar-agent claim`"));
}

TEST_CASE("a catalog example that names no linted binary is reported", "[cli_usage_lint][cli-usage][catalog]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root  repo{"catalog_nobin", schema_with_examples(R"("make test")")};
  auto const r = run_lint(repo);
  INFO(r.out);
  CHECK(r.code == 1);
  CHECK(contains(r.out, "has an example that names no linted binary: make test"));
}

TEST_CASE("the skills tree is a scan root, and skills/<name> is walked once", "[cli_usage_lint][cli-usage][skills]") {
  REQUIRE(fs::exists(PLANAR_CLI_USAGE_LINT_BIN));
  repo_root repo{"skillsroot"};

  SECTION("a bad flag in skills/<name>/SKILL.md and in its references is reported") {
    repo.write("skills/planar/SKILL.md", "---\nname: planar\n---\n\n```\nplanar-agent heartbeat --bogus -- make\n```\n");
    repo.write("skills/planar/references/claims.md", "# Claims\n\n```\nplanar-agent claim --nope\n```\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 1);
    CHECK(contains(r.out, repo.path("skills/planar/SKILL.md") + ":6: `planar-agent heartbeat` has no flag `--bogus`"));
    CHECK(contains(r.out, repo.path("skills/planar/references/claims.md") + ":4: `planar-agent claim` has no flag `--nope`"));
    CHECK(contains(r.out, "across 2 files"));
  }

  SECTION("clean skills trees pass and are counted") {
    repo.write("skills/planar/SKILL.md", "---\nname: planar\n---\n\n```\nplanar-agent heartbeat --claim t -- make\n```\n");
    auto const r = run_lint(repo);
    INFO(r.out);
    CHECK(r.code == 0);
    CHECK(contains(r.out, "cli-usage-lint: clean (1 files"));
  }
}
