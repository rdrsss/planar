// @file routing_build.t.cpp
// @brief Tests for the WRITE half of `planar.engine.workspace.routing`
// (plan 996, task 6275).
//
// ============================================================================
// THE FIXTURE IS DERIVED DATA, SO ITS OWN SHAPE IS ASSERTED FIRST
// ============================================================================
// Everything this module produces is computed from a scratch REPOSITORY on
// disk plus a scratch DATABASE. A fixture that silently matched nothing — a
// misplaced go.mod, a root_path pointing at an empty directory — would make
// every "the builder did not emit X" assertion below pass for the wrong
// reason, and a suite of such assertions reads exactly like a passing suite.
//
// So the first test asserts the fixture's OWN shape: that the repositories
// exist, that the builder finds all of them, and that each one produced the
// non-empty derived fields it was built to produce. Every later test can then
// treat an absence as meaningful.
//
// The same rule applies inside individual tests: wherever something is
// asserted ABSENT, the matching PRESENT case is asserted beside it.
//
// ============================================================================
// HOME SAFETY
// ============================================================================
// This module never reads the environment — `build` takes a connection and
// paths only. But it WRITES wherever `write_table` is pointed and READS
// whatever `projects.root_path` says, so every test here builds its own
// scratch tree, registers root paths INSIDE it, and writes only into it.
// Nothing reaches a real `~/.planar` because nothing here can name one.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Every expected value below was captured by RUNNING zig/zig-out/bin/planar
// in a pinned scratch arena with HOME / PLANAR_HOME / PLANAR_DB redirected
// into it, both streams through a PIPE, and the exit code read OUTSIDE the
// pipe. Beyond the per-case captures quoted at each test, the whole module
// was checked end to end by a DIFFERENTIAL: the same four-repository fixture
// (with operator overrides and a repo-scoped active plan) was built by the
// oracle and by this port, and the two routing-table.json files were diffed.
//
// The result, over 141 lines: byte-identical everywhere except the
// `languages` KEY ORDER, which is the recorded divergence (routing.cppm's
// header). Sorting the language keys on both sides made the diff clean, and
// perturbing one byte of the port's output made it fail again — so the
// comparison was not passing vacuously.

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.workspace;
import planar.json_dom;

#include <catch2/catch_test_macros.hpp>

namespace routing = planar::engine::workspace::routing;

namespace {

/// @brief A scratch tree holding the database and the fixture repositories.
struct scratch_tree {
  std::filesystem::path root_;

  scratch_tree()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_ws_build_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(root_ / "repos", ec);
    std::filesystem::create_directories(root_ / "state", ec);
  }
  scratch_tree(const scratch_tree&)            = delete;
  scratch_tree& operator=(const scratch_tree&) = delete;
  ~scratch_tree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] auto db_path() const -> std::filesystem::path {
    return root_ / "w.db";
  }
  [[nodiscard]] auto repo(std::string_view slug) const -> std::filesystem::path {
    return root_ / "repos" / slug;
  }
  [[nodiscard]] auto state() const -> std::filesystem::path {
    return root_ / "state";
  }
};

auto open_migrated(const scratch_tree& tree) -> planar::db::connection {
  auto conn = planar::db::connection::open(tree.db_path().string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

/// @brief Run one statement, reporting SQLite's own message on failure.
///
/// The message matters: a silent `has_value()` failure here says only "the
/// fixture did not seed", which sends the reader hunting through the builder
/// rather than at the column list two lines above.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto done = conn.execute(sql);
  if (!done.has_value()) {
    INFO("SQL: " << sql);
    INFO("sqlite: " << done.error().message_);
    FAIL();
  }
}

/// @brief Write `body` to `path`, creating parents.
auto put(const std::filesystem::path& path, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  REQUIRE(out.good());
  out << body;
}

/// @brief Register one org.
auto add_org(planar::db::connection& conn, std::string_view slug) -> void {
  auto stmt = conn.prepare("insert into associations (kind, slug, name) values ('org', ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Register a project and add it to org 1.
auto add_member(planar::db::connection& conn, std::string_view slug, const std::filesystem::path& root,
                std::string_view remote = "") -> void {
  auto stmt = conn.prepare("insert into projects (slug, name, root_path, git_remote) values (?, ?, ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->bind_text(3, root.string()).has_value());
  REQUIRE(stmt->bind_text(4, remote).has_value());
  REQUIRE(stmt->step().has_value());

  auto link = conn.prepare("insert into project_associations (project_id, association_id, source) "
                           "select id, 1, 'user' from projects where slug = ?");
  REQUIRE(link.has_value());
  REQUIRE(link->bind_text(1, slug).has_value());
  REQUIRE(link->step().has_value());
}

/// @brief The project entry for `slug`, or a failed REQUIRE.
auto project_named(const routing::routing_table& table, std::string_view slug) -> const routing::project_route& {
  const auto found = std::ranges::find(table.projects, slug, &routing::project_route::slug);
  REQUIRE(found != table.projects.end());
  return *found;
}

/// @brief The share recorded for `language`, or nullopt.
auto share_of(const routing::project_route& project, std::string_view language) -> std::optional<double> {
  const auto found = std::ranges::find(project.languages, language, &std::pair<std::string, double>::first);
  return found == project.languages.end() ? std::nullopt : std::optional{found->second};
}

/// @brief The language keys in emitted order.
auto language_keys(const routing::project_route& project) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& [language, _] : project.languages) {
    out.push_back(language);
  }
  return out;
}

/// @brief The four-repository fixture the differential was run against.
///
/// Built through the module's own inputs — files on disk and rows in the
/// database — never by constructing a `routing_table` directly, which would
/// make the tests agree with the builder by construction.
auto seed_fixture(const scratch_tree& tree, planar::db::connection& conn) -> void {
  add_org(conn, "acme");

  // alpha: a Go SERVICE (go.mod + a `package main` under cmd/), a README with
  // a heading paragraph followed by a two-line prose paragraph, and a go.mod
  // `replace` pointing at beta.
  put(tree.repo("alpha") / "README.md",
      "# Alpha\n\nAlpha is the front door.\nIt fans requests out.\n\nSecond paragraph, ignored.\n");
  put(tree.repo("alpha") / "go.mod",
      std::format("module example.com/alpha\n\ngo 1.22\n\nreplace example.com/beta => {}\n", (tree.repo("beta")).string()));
  put(tree.repo("alpha") / "cmd" / "alpha" / "main.go", "package main\n\nfunc main() {}\n");
  put(tree.repo("alpha") / "lib.go", "package alpha\n");

  // beta: a node package depending on alpha through a `workspace:` value, and
  // a README that is NOTHING BUT a heading — so its summary must come out
  // EMPTY.
  put(tree.repo("beta") / "package.json", R"({"name":"beta","dependencies":{"alpha":"workspace:*","react":"^18.0.0"},)"
                                          R"("devDependencies":{"express":"^4.0.0"}})");
  put(tree.repo("beta") / "src" / "index.ts", "export const x = 1;\n");
  put(tree.repo("beta") / "README.md", "# beta\n");
  put(tree.repo("beta") / "bin" / "betarun", "#!/bin/sh\n");

  // gamma: multi-language, no build files at all.
  put(tree.repo("gamma") / "a.js", "console.log(1)\n");
  put(tree.repo("gamma") / "b.js", "console.log(2)\n");
  put(tree.repo("gamma") / "c.md", "# doc\n");
  put(tree.repo("gamma") / "d.json", "{}\n");
  put(tree.repo("gamma") / "e.py", "print(1)\n");

  // delta: one file, and its extension is not a known language.
  put(tree.repo("delta") / "only.bin", "x\n");

  for (const auto* slug : {"alpha", "beta", "gamma", "delta"}) {
    add_member(conn, slug, tree.repo(slug));
  }
}

} // namespace

// ===========================================================================
// The fixture's own shape — FIRST, so every later absence means something
// ===========================================================================

TEST_CASE("the build fixture produces a non-empty table with every derived field populated",
          "[engine][workspace][routing][build][fixture]") {
  // This test exists to fail LOUDLY if the fixture stops matching. Without
  // it, a misplaced go.mod or a root_path pointing nowhere would turn every
  // negative assertion in this file into a vacuous pass.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  // The repositories are really on disk.
  for (const auto* slug : {"alpha", "beta", "gamma", "delta"}) {
    INFO("fixture repo: " << slug);
    CHECK(std::filesystem::is_directory(tree.repo(slug)));
  }

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  // All four members were found, in slug order.
  REQUIRE(table->projects.size() == 4);
  CHECK(table->projects[0].slug == "alpha");
  CHECK(table->projects[1].slug == "beta");
  CHECK(table->projects[2].slug == "delta");
  CHECK(table->projects[3].slug == "gamma");

  // Each derived field that the fixture was built to populate IS populated,
  // so its absence elsewhere is a finding rather than an accident.
  CHECK_FALSE(project_named(*table, "alpha").summary.empty());
  CHECK_FALSE(project_named(*table, "alpha").capabilities.empty());
  CHECK_FALSE(project_named(*table, "alpha").depends_on.empty());
  CHECK_FALSE(project_named(*table, "alpha").entry_points.empty());
  CHECK_FALSE(project_named(*table, "alpha").languages.empty());
  CHECK_FALSE(project_named(*table, "beta").capabilities.empty());
  CHECK_FALSE(project_named(*table, "gamma").languages.empty());
  CHECK_FALSE(table->cross.dependency_edges.empty());

  // And the header fields the caller supplied survived.
  CHECK(table->schema_version == 1);
  CHECK(table->workspace_id == 1);
  CHECK(table->workspace_slug == "acme");
  CHECK(table->workspace_name == "Acme");
  CHECK(table->generator_version == "static-v1");
  // The stamp comes from SQLite, so it is not empty and it is not a host
  // clock format of some other shape.
  CHECK(table->generated_at.size() == 20);
  CHECK(table->generated_at.back() == 'Z');
}

// ===========================================================================
// Capabilities
// ===========================================================================

TEST_CASE("build detects capabilities and suppresses go-library under go-service", "[engine][workspace][routing][build]") {
  // Oracle-captured on this fixture: alpha reports ["go-service"] alone even
  // though the `go-library` rule ALSO matches its go.mod. beta reports
  // ["node-service","react-app"] because its package.json declares both
  // `express` and `react`.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  CHECK(project_named(*table, "alpha").capabilities == std::vector<std::string>{"go-service"});
  CHECK(project_named(*table, "alpha").capabilities_source == "static");
  CHECK(project_named(*table, "beta").capabilities == std::vector<std::string>{"node-service", "react-app"});

  // The ABSENT case, with its PRESENT partner asserted above: gamma has no
  // build file of any kind, so it earns nothing AND its source is empty
  // rather than "static".
  CHECK(project_named(*table, "gamma").capabilities.empty());
  CHECK(project_named(*table, "gamma").capabilities_source.empty());
}

TEST_CASE("go-library survives when there is no package main", "[engine][workspace][routing][build]") {
  // The discriminating half of the suppression rule above. Without this, a
  // port that dropped `go-library` unconditionally would pass that test.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  put(tree.repo("lib") / "go.mod", "module example.com/lib\n");
  put(tree.repo("lib") / "lib.go", "package lib\n");
  add_member(conn, "lib", tree.repo("lib"));

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  CHECK(project_named(*table, "lib").capabilities == std::vector<std::string>{"go-library"});
}

TEST_CASE("a capability pattern containing a slash scans that subdirectory", "[engine][workspace][routing][build][6322]") {
  // INVERTED AT TASK 6322 (decision 1067's FIX set). This case used to assert
  // the opposite -- that `proto/*.proto` never matches -- pinning a genuine
  // use-after-free reproduced from the oracle: `patternFinds` freed the joined
  // path before `openDir` saw it and the `catch return false` swallowed it.
  // Every slash-bearing capability pattern silently matched nothing,
  // including the SHIPPED protobuf rule's `proto/*.proto` arm.
  //
  // Reproducing it was right while the oracle existed (D2) because the fix
  // emits tags the oracle never emits on a persisted write path. Decision
  // 1067 ended that rule, and named this row in its FIX set.
  //
  // Both halves still matter: the subdirectory match is only meaningful
  // beside the root match, which never regressed.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  put(tree.repo("rootproto") / "a.proto", "syntax=\"proto3\";\n");
  put(tree.repo("subproto") / "proto" / "a.proto", "syntax=\"proto3\";\n");
  add_member(conn, "rootproto", tree.repo("rootproto"));
  add_member(conn, "subproto", tree.repo("subproto"));

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  // The no-slash `*.proto` arm fires at the repo root, as it always did.
  CHECK(project_named(*table, "rootproto").capabilities == std::vector<std::string>{"protobuf"});
  // And the `proto/*.proto` arm now fires in the subdirectory.
  CHECK(project_named(*table, "subproto").capabilities == std::vector<std::string>{"protobuf"});
}

TEST_CASE("a slash pattern does NOT match the same filename at the root", "[engine][workspace][routing][build][6322]") {
  // Non-vacuity for the case above. Without this, a "fix" that ignored the
  // directory part entirely -- globbing the leaf against the ROOT -- would
  // satisfy it. `proto/*.proto` must look in `proto/`, not everywhere.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  // A .proto at the root of a repo that has NO proto/ directory. The
  // shipped rule's other arm (`*.proto`, no slash) is what should fire here;
  // to isolate the slash arm this case uses a rules set carrying ONLY it.
  put(tree.repo("flat") / "a.proto", "syntax=\"proto3\";\n");
  add_member(conn, "flat", tree.repo("flat"));

  std::vector<routing::capability_rule> only_slash{
      routing::capability_rule{.tag = "protobuf", .match_all = {}, .match_any = {"proto/*.proto"}}};
  auto table = routing::build(conn, 1, "acme", "Acme", only_slash);
  REQUIRE(table.has_value());
  CHECK(project_named(*table, "flat").capabilities.empty());
}

TEST_CASE("a traversing slash pattern is refused, not followed", "[engine][workspace][routing][build][6322]") {
  // The scan is rooted at a project directory and the rules file is
  // operator-authored, so `..` is refused rather than normalized away.
  // Silently clamping it would hide a rules-file bug the same way the
  // original defect hid itself.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  put(tree.repo("esc") / "a.proto", "syntax=\"proto3\";\n");
  add_member(conn, "esc", tree.repo("esc"));

  std::vector<routing::capability_rule> traversing{
      routing::capability_rule{.tag = "protobuf", .match_all = {}, .match_any = {"../esc/*.proto"}}};
  auto table = routing::build(conn, 1, "acme", "Acme", traversing);
  REQUIRE(table.has_value());
  CHECK(project_named(*table, "esc").capabilities.empty());
}

TEST_CASE("a rule with both pattern lists empty matches nothing", "[engine][workspace][routing][build]") {
  // Oracle-captured with a bare `[[rule]]` header: capabilities came back
  // empty rather than universal. The discriminating case is the same rule
  // with one pattern added, which DOES fire.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  put(tree.repo("any") / "f.go", "package any\n");
  add_member(conn, "any", tree.repo("any"));

  const std::vector<routing::capability_rule> inert{{.tag = "inert"}};
  auto                                        empty = routing::build(conn, 1, "acme", "Acme", inert);
  REQUIRE(empty.has_value());
  CHECK(project_named(*empty, "any").capabilities.empty());

  const std::vector<routing::capability_rule> live{{.tag = "live", .match_all = {"*.go"}}};
  auto                                        fired = routing::build(conn, 1, "acme", "Acme", live);
  REQUIRE(fired.has_value());
  CHECK(project_named(*fired, "any").capabilities == std::vector<std::string>{"live"});
}

// ===========================================================================
// Cross-repo dependencies
// ===========================================================================

TEST_CASE("build infers dependencies from go.mod and package.json, and records which", "[engine][workspace][routing][build]") {
  // Oracle-captured: alpha depends on beta with source `go.mod`, beta on
  // alpha with source `package.json`, and the two edges carry DIFFERENT
  // reasons derived from those sources.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  CHECK(project_named(*table, "alpha").depends_on == std::vector<std::string>{"beta"});
  CHECK(project_named(*table, "alpha").depends_on_source == "go.mod");
  CHECK(project_named(*table, "beta").depends_on == std::vector<std::string>{"alpha"});
  CHECK(project_named(*table, "beta").depends_on_source == "package.json");
  // ABSENT, with the present case above: gamma has neither file.
  CHECK(project_named(*table, "gamma").depends_on.empty());
  CHECK(project_named(*table, "gamma").depends_on_source.empty());

  REQUIRE(table->cross.dependency_edges.size() == 2);
  // Sorted by (from, to).
  CHECK(table->cross.dependency_edges[0].from == "alpha");
  CHECK(table->cross.dependency_edges[0].to == "beta");
  CHECK(table->cross.dependency_edges[0].reason == "go.mod replace");
  CHECK(table->cross.dependency_edges[1].from == "beta");
  CHECK(table->cross.dependency_edges[1].to == "alpha");
  CHECK(table->cross.dependency_edges[1].reason == "package.json workspace dep");
}

TEST_CASE("when BOTH sources contribute, depends_on_source stays go.mod", "[engine][workspace][routing][build]") {
  // A TEST-GAP FIX, found by break-probe rather than by reading. Relaxing
  // `if (source.empty() && !deps.empty())` to `if (!deps.empty())` in the
  // package.json arm SURVIVED the suite above, because no fixture project
  // had both files — alpha has a go.mod and no package.json, beta the
  // reverse. So the precedence rule was asserted nowhere.
  //
  // The original sets `source` inside the go.mod sibling loop and only
  // DEFAULTS it in the package.json arm, so go.mod wins whenever both
  // contribute. That is observable twice over: in `depends_on_source`, and
  // in every `dependency_edges.reason` derived from it.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");

  // `both` depends on `x` through go.mod AND on `y` through package.json.
  put(tree.repo("both") / "go.mod",
      std::format("module example.com/both\n\nreplace example.com/x => {}\n", tree.repo("x").string()));
  put(tree.repo("both") / "package.json", R"({"name":"both","dependencies":{"y":"workspace:*"}})");
  put(tree.repo("x") / "keep.go", "package x\n");
  put(tree.repo("y") / "keep.go", "package y\n");
  for (const auto* slug : {"both", "x", "y"}) {
    add_member(conn, slug, tree.repo(slug));
  }

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  const auto& both = project_named(*table, "both");
  // BOTH dependencies are collected — the package.json arm still runs.
  CHECK(both.depends_on == std::vector<std::string>{"x", "y"});
  // But the source is the FIRST contributor, not the last.
  CHECK(both.depends_on_source == "go.mod");
  // And that propagates into the edge reasons, which is where an operator
  // would actually see it.
  REQUIRE(table->cross.dependency_edges.size() == 2);
  CHECK(table->cross.dependency_edges[0].reason == "go.mod replace");
  CHECK(table->cross.dependency_edges[1].reason == "go.mod replace");
}

TEST_CASE("a go.mod replace pointing outside the workspace yields no dependency", "[engine][workspace][routing][build]") {
  // The discriminating half of the go.mod arm: the right-hand side is matched
  // against a sibling's recorded root_path, so a path that resolves nowhere
  // near one contributes nothing AND leaves the source empty.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  put(tree.repo("solo") / "go.mod", "module example.com/solo\n\nreplace example.com/x => ../nowhere\n");
  add_member(conn, "solo", tree.repo("solo"));

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  CHECK(project_named(*table, "solo").depends_on.empty());
  CHECK(project_named(*table, "solo").depends_on_source.empty());
}

// ===========================================================================
// Summary
// ===========================================================================

TEST_CASE("the summary is the first PROSE paragraph, and a heading-only README yields none",
          "[engine][workspace][routing][build]") {
  // Both halves oracle-captured on this fixture. alpha's README opens with a
  // heading paragraph, then a two-line paragraph that is flattened with a
  // single space; the third paragraph is never reached. beta's README is
  // nothing BUT a heading, so both the summary and its source come back
  // empty — note the source is keyed on the SUMMARY, not on a README
  // existing, and beta's README does exist.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  CHECK(project_named(*table, "alpha").summary == "Alpha is the front door. It fans requests out.");
  CHECK(project_named(*table, "alpha").summary_source == "readme");

  CHECK(std::filesystem::exists(tree.repo("beta") / "README.md"));
  CHECK(project_named(*table, "beta").summary.empty());
  CHECK(project_named(*table, "beta").summary_source.empty());
}

TEST_CASE("a heading with prose ATTACHED beneath it contributes only the prose", "[engine][workspace][routing][build]") {
  // A TEST-GAP FIX, found by break-probe. Changing `start = 1` to `start = 0`
  // — i.e. no longer stripping the heading line — SURVIVED the suite, because
  // that branch is only reachable for a MULTI-LINE paragraph whose first line
  // is a heading, and no fixture had one. The alpha README separates its
  // heading from its prose with a blank line, so the heading forms its own
  // single-line paragraph and is skipped by a different rule entirely.
  //
  // All four shapes below were then captured from the oracle rather than
  // reasoned about, and one of them is not what reasoning would predict.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");

  // The branch under test: heading, then prose, no blank line.
  put(tree.repo("attached") / "README.md", "# Title\nImmediately following prose.\nMore prose.\n\nLater paragraph.\n");
  // THE SURPRISE: a paragraph of two heading lines. The FIRST is stripped and
  // the second is not tested at all, so the summary is literally `## Two` —
  // the heading rule fires once per paragraph, not once per line.
  put(tree.repo("hashonly") / "README.md", "# One\n## Two\n\nReal prose here.\n");
  // The heading test runs against the TRIMMED line, so indentation does not
  // hide a heading.
  put(tree.repo("indented") / "README.md", "   # Indented heading\n   Prose after it.\n");
  // The name list falls through `README.md` to a bare `README`.
  put(tree.repo("fallback") / "README", "Bare readme prose.\n");

  for (const auto* slug : {"attached", "hashonly", "indented", "fallback"}) {
    add_member(conn, slug, tree.repo(slug));
  }

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  CHECK(project_named(*table, "attached").summary == "Immediately following prose. More prose.");
  CHECK(project_named(*table, "hashonly").summary == "## Two");
  CHECK(project_named(*table, "indented").summary == "Prose after it.");
  CHECK(project_named(*table, "fallback").summary == "Bare readme prose.");
  // All four are `readme`-sourced, which is what distinguishes "found prose"
  // from "found nothing and defaulted to empty".
  for (const auto* slug : {"attached", "hashonly", "indented", "fallback"}) {
    INFO("summary_source for " << slug);
    CHECK(project_named(*table, slug).summary_source == "readme");
  }
}

// ===========================================================================
// Entry points
// ===========================================================================

TEST_CASE("entry points are sorted and capped at five AFTER sorting", "[engine][workspace][routing][build]") {
  // Oracle-captured on a repository with nine candidates: it kept
  // Cargo.toml, Makefile, bin/zz, cmd/aa/main.go, cmd/bb/main.go and dropped
  // go.mod, main.py, manage.py, package.json, pyproject.toml, src/index.ts.
  // The cap is applied AFTER the sort, which is what makes the survivors
  // deterministic — a port that truncated first would keep a different five.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  const auto root = tree.repo("many");
  put(root / "go.mod", "module m\n");
  put(root / "package.json", "{}\n");
  put(root / "Cargo.toml", "[p]\n");
  put(root / "Makefile", "all:\n");
  put(root / "main.py", "x\n");
  put(root / "manage.py", "x\n");
  put(root / "pyproject.toml", "[p]\n");
  put(root / "src" / "index.ts", "export const a = 1\n");
  put(root / "cmd" / "aa" / "main.go", "package main\n");
  put(root / "cmd" / "bb" / "main.go", "package main\n");
  put(root / "bin" / "zz", "x\n");
  add_member(conn, "many", root);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  CHECK(project_named(*table, "many").entry_points ==
        std::vector<std::string>{"Cargo.toml", "Makefile", "bin/zz", "cmd/aa/main.go", "cmd/bb/main.go"});
}

// ===========================================================================
// Languages
// ===========================================================================

TEST_CASE("language shares are file counts rounded to two places", "[engine][workspace][routing][build]") {
  // Oracle-captured shares on this fixture: alpha counts two .go files and
  // one .md (go.mod has no recognised extension), so go is 0.67 and markdown
  // 0.33; gamma counts five files, two of them .js, so javascript is 0.4 and
  // the other three languages 0.2 each.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  const auto& alpha = project_named(*table, "alpha");
  // The denominator is the count of files that MATCHED a language, not every
  // file visited: alpha holds four files, and go.mod is not one of the three
  // that count (`.mod` is not in the extension table). So the shares are over
  // 3, not 4 — 0.67 rather than 0.5.
  CHECK(share_of(alpha, "go") == 0.67);
  CHECK(share_of(alpha, "markdown") == 0.33);
  CHECK_FALSE(share_of(alpha, "toml").has_value());

  const auto& gamma = project_named(*table, "gamma");
  CHECK(share_of(gamma, "javascript") == 0.4);
  CHECK(share_of(gamma, "markdown") == 0.2);
  CHECK(share_of(gamma, "python") == 0.2);
  CHECK(share_of(gamma, "json") == 0.2);

  // ABSENT, beside the present cases above: delta's one file has an
  // unrecognised extension, so its census is empty rather than a map of
  // zeroes.
  CHECK(project_named(*table, "delta").languages.empty());
}

TEST_CASE("language keys are emitted SORTED, the port's one deliberate divergence",
          "[engine][workspace][routing][build][divergence]") {
  // DIVERGENCE 1. The oracle emits this map in Zig hash order — captured as
  // `typescript, markdown, json` for beta and `javascript, markdown, python,
  // json` for gamma in a single build, neither sorted nor frequency-ordered.
  // That order is a hash seed and is not reproducible, so this builder sorts
  // and the SORTED order is what is pinned here.
  //
  // Deliberately NOT written as "equals the captured oracle sequence": there
  // is no parity to assert on this field. See routing.cppm's header.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  const auto keys = language_keys(project_named(*table, "gamma"));
  CHECK(keys == std::vector<std::string>{"javascript", "json", "markdown", "python"});
  CHECK(std::ranges::is_sorted(keys));
  // And the sortedness is a property of the emitter, not of this one
  // fixture's alphabet.
  CHECK(std::ranges::is_sorted(language_keys(project_named(*table, "beta"))));
}

TEST_CASE("a share below half a percent rounds to zero and is still emitted", "[engine][workspace][routing][build]") {
  // Oracle-captured: a 400-file repository with one Go file emitted
  // `"go": 0, "markdown": 1`. The language is NOT dropped, and the majority
  // share prints as a whole number.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  put(tree.repo("tiny") / "one.go", "package t\n");
  for (int i = 0; i < 399; ++i) {
    put(tree.repo("tiny") / std::format("m{}.md", i), "# m\n");
  }
  add_member(conn, "tiny", tree.repo("tiny"));

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  CHECK(share_of(project_named(*table, "tiny"), "go") == 0.0);
  CHECK(share_of(project_named(*table, "tiny"), "markdown") == 1.0);
  // Emitted, not dropped — the distinction this test exists for.
  CHECK(project_named(*table, "tiny").languages.size() == 2);
}

TEST_CASE("dot-directories and vendor trees are skipped, dot-FILES are not", "[engine][workspace][routing][build]") {
  // The skip list covers `.`-prefixed directories plus node_modules, vendor,
  // target, dist and build. It does NOT cover dot-files, and zig's
  // `extension` gives a leading-dot name no extension at all — so
  // `.hidden.go` counts and `.gitignore` does not.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  const auto root = tree.repo("skips");
  put(root / "kept.go", "package k\n");
  put(root / ".hidden.go", "package k\n");
  put(root / "kept.md", "# k\n");
  put(root / ".gitignore", "x\n");
  put(root / ".git" / "buried.go", "package k\n");
  put(root / "node_modules" / "buried.go", "package k\n");
  put(root / "vendor" / "buried.go", "package k\n");
  put(root / "target" / "buried.go", "package k\n");
  put(root / "dist" / "buried.go", "package k\n");
  put(root / "build" / "buried.go", "package k\n");
  add_member(conn, "skips", root);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  const auto& skips = project_named(*table, "skips");
  // Counted: kept.go and .hidden.go (a dot-FILE with a real extension) plus
  // kept.md. `.gitignore` is visited but contributes nothing — zig gives a
  // leading-dot name NO extension — and every buried.go is unreachable
  // because its directory is skipped. So go is 2/3, not 2/2 and not 8/9.
  CHECK(share_of(skips, "go") == 0.67);
  CHECK(share_of(skips, "markdown") == 0.33);
  // The discriminating assertion: if any skipped directory HAD been walked
  // the go share would be higher, so this pins the skipping rather than
  // merely the arithmetic.
  CHECK(skips.languages.size() == 2);
}

// ===========================================================================
// planar_focus and cross_repo
// ===========================================================================

TEST_CASE("planar_focus counts repo-scoped work and follows touches for plans only", "[engine][workspace][routing][build]") {
  // The asymmetry is the point: `active_plans` unions the repo-scoped arm
  // with a `touches` edge, while the task and question counts are
  // repo-scope ONLY. So a plan can appear through `touches` while
  // contributing nothing to either count.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  exec(conn, "insert into plans (title, slug, status, scope_kind, scope_id) "
             "values ('direct', 'direct', 'active', 'repo', 1)");
  exec(conn, "insert into plans (title, slug, status, scope_kind, scope_id) "
             "values ('drafty', 'drafty', 'draft', 'repo', 1)");
  exec(conn, "insert into plans (title, slug, status, scope_kind, scope_id) "
             "values ('toucher', 'toucher', 'active', 'repo', 2)");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('plan', 3, 'repo', 1, 'touches')");
  exec(conn, "insert into tasks (title, slug, status, scope_kind, scope_id) "
             "values ('t1', 't1', 'todo', 'repo', 1)");
  exec(conn, "insert into tasks (title, slug, status, scope_kind, scope_id) "
             "values ('t2', 't2', 'blocked', 'repo', 1)");
  exec(conn, "insert into tasks (title, slug, status, scope_kind, scope_id) "
             "values ('t3', 't3', 'done', 'repo', 1)");
  exec(conn, "insert into questions (title, status, scope_kind, scope_id) "
             "values ('q1', 'open', 'repo', 1)");
  // An `answered` question needs both answer columns — the schema enforces it.
  exec(conn, "insert into questions (title, status, scope_kind, scope_id, answer_body, answered_at) "
             "values ('q2', 'answered', 'repo', 1, 'a', 'T')");

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());

  const auto& focus = project_named(*table, "alpha").focus;
  // Plan 1 directly, plan 3 through `touches`. Plan 2 is a DRAFT and is
  // excluded, which is what keeps this from being "every plan on the repo".
  CHECK(focus.active_plans == std::vector<std::int64_t>{1, 3});
  // todo + blocked, not done.
  CHECK(focus.open_tasks == 2);
  // open, not answered.
  CHECK(focus.open_questions == 1);
  // Always empty, in the oracle too.
  CHECK(focus.recent_session_ids.empty());

  // The touching plan did NOT drag its scope's tasks along: beta (project 2)
  // has none of its own.
  CHECK(project_named(*table, "beta").focus.open_tasks == 0);
}

TEST_CASE("cross_repo collects org-scoped plans and OPEN questions only", "[engine][workspace][routing][build]") {
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);

  // Plans are collected regardless of status; questions only when open.
  exec(conn, "insert into plans (title, slug, status, scope_kind, scope_id) "
             "values ('orgp', 'orgp', 'draft', 'association', 1)");
  exec(conn, "insert into questions (title, status, scope_kind, scope_id) "
             "values ('oq', 'open', 'association', 1)");
  exec(conn, "insert into questions (title, status, scope_kind, scope_id, answer_body, answered_at) "
             "values ('aq', 'answered', 'association', 1, 'a', 'T')");

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  CHECK(table->cross.plans_scoped_to_org == std::vector<std::int64_t>{1});
  // PRESENT and ABSENT together: the open one is listed, the answered one is
  // not, so the list is not simply "every question".
  CHECK(table->cross.questions_scoped_to_org == std::vector<std::int64_t>{1});
}

TEST_CASE("a member whose root_path does not exist yields an EMPTY entry, not an error", "[engine][workspace][routing][build]") {
  // Oracle-captured against a project registered and then deleted from disk:
  // the build succeeded and the entry carried empty everything.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  add_org(conn, "acme");
  add_member(conn, "ghost", tree.repo("ghost"));
  REQUIRE_FALSE(std::filesystem::exists(tree.repo("ghost")));

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  const auto& ghost = project_named(*table, "ghost");
  CHECK(ghost.summary.empty());
  CHECK(ghost.capabilities.empty());
  CHECK(ghost.entry_points.empty());
  CHECK(ghost.languages.empty());
  // The row's own columns still came through, which is what distinguishes
  // "empty because nothing was found" from "empty because nothing was read".
  CHECK(ghost.slug == "ghost");
  CHECK(ghost.root_path == tree.repo("ghost").string());
}

// ===========================================================================
// The capability-rules file
// ===========================================================================

TEST_CASE("load_capability_rules parses a rules file and replaces the defaults", "[engine][workspace][routing][build][rules]") {
  // Oracle-captured: a file declaring one `docs-only` rule produced exactly
  // that tag and NO built-in tag, so the file replaces rather than extends.
  scratch_tree tree;
  put(tree.state() / "rules.toml", "# a comment\n"
                                   "[[rule]]\n"
                                   "tag = \"docs-only\"\n"
                                   "match_any = [\"*.md\", \"*.markdown\"]\n"
                                   "\n"
                                   "[[rule]]\n"
                                   "tag = \"gofile\"\n"
                                   "match_all = ['*.go']\n"
                                   "go_main = true\n"
                                   "package_dep = \"react\"\n");

  auto rules = routing::load_capability_rules(tree.state() / "rules.toml");
  REQUIRE(rules.has_value());
  REQUIRE(rules->size() == 2);
  CHECK((*rules)[0].tag == "docs-only");
  CHECK((*rules)[0].match_any == std::vector<std::string>{"*.md", "*.markdown"});
  CHECK((*rules)[0].match_all.empty());
  CHECK_FALSE((*rules)[0].go_main);
  CHECK((*rules)[1].tag == "gofile");
  // Single quotes are accepted INSIDE an array.
  CHECK((*rules)[1].match_all == std::vector<std::string>{"*.go"});
  CHECK((*rules)[1].go_main);
  CHECK((*rules)[1].package_dep == "react");
}

TEST_CASE("an absent rules file, and one with no rule header, both load EMPTY", "[engine][workspace][routing][build][rules]") {
  // Both are how the caller learns to fall back to the built-ins.
  // Oracle-captured: a file of key/value lines with no `[[rule]]` header
  // produced the DEFAULT capability set, not an empty one.
  scratch_tree tree;
  auto         missing = routing::load_capability_rules(tree.state() / "nope.toml");
  REQUIRE(missing.has_value());
  CHECK(missing->empty());

  put(tree.state() / "headless.toml", "tag = \"x\"\nmatch_all = [\"*.go\"]\n");
  auto headless = routing::load_capability_rules(tree.state() / "headless.toml");
  REQUIRE(headless.has_value());
  CHECK(headless->empty());

  // The discriminating case: WITH a header the same lines do produce a rule,
  // so "empty" above is about the header and not about the parser failing.
  put(tree.state() / "headed.toml", "[[rule]]\ntag = \"x\"\nmatch_all = [\"*.go\"]\n");
  auto headed = routing::load_capability_rules(tree.state() / "headed.toml");
  REQUIRE(headed.has_value());
  CHECK(headed->size() == 1);
}

TEST_CASE("the rules parser keeps ParseFailed and InvalidInput apart", "[engine][workspace][routing][build][rules]") {
  // TWO EXIT CODES behind one message template, and which arm a malformed
  // line takes is NOT derivable from its shape — `match_all = 1.5` fails to
  // parse while `match_all = 7` parses and is merely the wrong type. Every
  // row below was captured from the oracle one line at a time.
  scratch_tree tree;

  const auto load = [&](std::string_view label, std::string_view body) {
    put(tree.state() / "r.toml", body);
    return routing::load_capability_rules(tree.state() / "r.toml");
  };

  // ParseFailed — exit 1.
  for (const auto* body : {"[[rule]]\ntag = \"x\"\nmatch_all = [oops]\n", "[[rule]]\ntag = \"x\"\nmatch_all = [\"a\"\n",
                           "[[rule]]\ntag = \"x\"\nmatch_all = 1.5\n", "[[rule]]\ntag = \"x\"\nmatch_all = [\"a\"] zz\n"}) {
    INFO("expected ParseFailed: " << body);
    auto got = load("parse", body);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == routing::rules_error::parse_failed);
    CHECK(routing::rules_error_name(got.error()) == "ParseFailed");
  }

  // InvalidInput — exit 2. Note `tag = 'x'` is here while `match_all = ['x']`
  // succeeds above: single quotes are legal in an ARRAY and not in a SCALAR.
  for (const auto* body : {"[[rule]]\ntag = \"x\"\nmatch_all = \"a\"\n", "[[rule]]\ntag = \"x\"\nmatch_all = 7\n",
                           "[[rule]]\ntag = \"x\"\nmatch_all = true\n", "[[rule]]\ntag = \"x\"\nmatch_all\n",
                           "[[rule]]\ntag = unquoted\n", "[[rule]]\ntag\n", "[[rule]]\ntag = 'x'\n"}) {
    INFO("expected InvalidInput: " << body);
    auto got = load("invalid", body);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == routing::rules_error::invalid);
    CHECK(routing::rules_error_name(got.error()) == "InvalidInput");
  }

  // And an EMPTY array is not an error at all — it produces an inert rule.
  auto empty_array = load("ok", "[[rule]]\ntag = \"x\"\nmatch_all = []\n");
  REQUIRE(empty_array.has_value());
  REQUIRE(empty_array->size() == 1);
  CHECK((*empty_array)[0].match_all.empty());
}

TEST_CASE("the rules parser matches keys by PREFIX and tests go_main as a SUBSTRING",
          "[engine][workspace][routing][build][rules]") {
  // Both oracle-captured, and both are the kind of thing a reasonable
  // re-implementation tightens by accident: `tagx = "x"` sets `tag`, and
  // `go_main = "untrue"` ENABLES the check because the value contains
  // `true`.
  scratch_tree tree;
  put(tree.state() / "r.toml", "[[rule]]\ntagx = \"fromprefix\"\ngo_main = \"untrue\"\nmatch_all = [\"*.go\"]\n");
  auto rules = routing::load_capability_rules(tree.state() / "r.toml");
  REQUIRE(rules.has_value());
  REQUIRE(rules->size() == 1);
  CHECK((*rules)[0].tag == "fromprefix");
  CHECK((*rules)[0].go_main);

  // The discriminating case, so "substring" is not confused with "always
  // true": a value with no `true` in it leaves the flag off.
  put(tree.state() / "r2.toml", "[[rule]]\ntag = \"t\"\ngo_main = false\nmatch_all = [\"*.go\"]\n");
  auto off = routing::load_capability_rules(tree.state() / "r2.toml");
  REQUIRE(off.has_value());
  REQUIRE(off->size() == 1);
  CHECK_FALSE((*off)[0].go_main);
}

TEST_CASE("a comment truncates the line it appears on, wherever it appears", "[engine][workspace][routing][build][rules]") {
  // Oracle-captured: `tag = "c" # trailing` yields the tag `c`. The
  // consequence worth naming is that no pattern may contain a `#` at all.
  scratch_tree tree;
  put(tree.state() / "r.toml", "[[rule]]\ntag = \"c\" # trailing\nmatch_all = [\"*.go\"]\n");
  auto rules = routing::load_capability_rules(tree.state() / "r.toml");
  REQUIRE(rules.has_value());
  REQUIRE(rules->size() == 1);
  CHECK((*rules)[0].tag == "c");
}

// ===========================================================================
// Overrides
// ===========================================================================

TEST_CASE("overrides replace fields and flip each source to manual", "[engine][workspace][routing][build][overrides]") {
  // Oracle-captured on a two-project fixture. Note the THIRD state: an empty
  // capabilities ARRAY still counts as present, so it clears the tags AND
  // sets the source to `manual` — that is the row a plausible port gets
  // wrong, because "no capabilities" and "no override" look alike.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);
  put(tree.state() / "ov.json", R"({"schema_version": 1,
 "projects": {
   "alpha": {"summary": "Overridden.", "capabilities": ["zeta","alpha","alpha"], "depends_on": ["gamma"]},
   "beta":  {"capabilities": []},
   "nosuch":{"summary": "ignored"}
 }})");

  auto values = routing::load_overrides(tree.state() / "ov.json");
  REQUIRE(values.has_value());
  CHECK(values->schema_version == 1);

  auto table = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(table.has_value());
  // Pre-state, so the post-state below is a change rather than a coincidence.
  CHECK(project_named(*table, "alpha").capabilities == std::vector<std::string>{"go-service"});
  CHECK(project_named(*table, "beta").capabilities_source == "static");

  routing::apply_overrides(*table, *values);

  const auto& alpha = project_named(*table, "alpha");
  CHECK(alpha.summary == "Overridden.");
  CHECK(alpha.summary_source == "manual");
  // Sorted and deduped on the way in.
  CHECK(alpha.capabilities == std::vector<std::string>{"alpha", "zeta"});
  CHECK(alpha.capabilities_source == "manual");
  CHECK(alpha.depends_on == std::vector<std::string>{"gamma"});
  CHECK(alpha.depends_on_source == "manual");

  const auto& beta = project_named(*table, "beta");
  CHECK(beta.capabilities.empty());
  CHECK(beta.capabilities_source == "manual");
  // An ABSENT key leaves its field alone — beta's summary and deps were not
  // named and must survive untouched.
  CHECK(beta.depends_on == std::vector<std::string>{"alpha"});
  CHECK(beta.depends_on_source == "package.json");

  // A project named in the file but absent from the workspace is ignored
  // rather than added.
  CHECK(table->projects.size() == 4);
  CHECK(std::ranges::none_of(table->projects, [](const routing::project_route& p) { return p.slug == "nosuch"; }));
}

TEST_CASE("load_overrides keeps its parse failures apart, and an absent file is empty",
          "[engine][workspace][routing][build][overrides]") {
  // Oracle-captured: `not json` exits 1 as SyntaxError, `[]` exits 2 as
  // InvalidInput. Same two-code split as the table decoder.
  scratch_tree tree;
  auto         missing = routing::load_overrides(tree.state() / "nope.json");
  REQUIRE(missing.has_value());
  CHECK(missing->projects.empty());

  put(tree.state() / "bad.json", "not json\n");
  auto syntax = routing::load_overrides(tree.state() / "bad.json");
  REQUIRE_FALSE(syntax.has_value());
  CHECK(syntax.error() == routing::decode_error::syntax);

  put(tree.state() / "arr.json", "[]\n");
  auto invalid = routing::load_overrides(tree.state() / "arr.json");
  REQUIRE_FALSE(invalid.has_value());
  CHECK(invalid.error() == routing::decode_error::invalid);
}

// ===========================================================================
// Encoding and the round trip
// ===========================================================================

TEST_CASE("encode reproduces the oracle's exact bytes", "[engine][workspace][routing][build][encode]") {
  // Captured verbatim from a routing-table.json the oracle wrote. Typed out
  // rather than round-tripped through this module's own encoder, which would
  // agree with it by construction.
  //
  // The shapes that matter and are not guessable: `[]` and `{}` collapse
  // inline while a non-empty container always breaks, a whole share prints
  // `1` rather than `1.0`, and there is NO trailing newline (write_table
  // appends it).
  routing::routing_table table{
      .schema_version    = 1,
      .workspace_id      = 7,
      .workspace_slug    = "acme",
      .workspace_name    = "Acme",
      .generated_at      = "2026-01-01T00:00:00Z",
      .generator_version = "static-v1",
      .projects          = {routing::project_route{
          .slug                = "solo",
          .root_path           = "/r/solo",
          .git_remote          = "",
          .summary             = "",
          .summary_source      = "",
          .capabilities        = {},
          .capabilities_source = "",
          .depends_on          = {"beta"},
          .depends_on_source   = "go.mod",
          .entry_points        = {},
          .languages           = {{"go", 1.0}},
          .focus = routing::planar_focus{.active_plans = {3}, .open_tasks = 0, .open_questions = 0, .recent_session_ids = {}}}},
      .cross             = routing::cross_repo{.plans_scoped_to_org     = {},
                                               .questions_scoped_to_org = {1},
                                               .dependency_edges        = {routing::dependency_edge{"solo", "beta", "go.mod replace"}}},
  };

  constexpr std::string_view k_expected = R"({
  "schema_version": 1,
  "workspace_id": 7,
  "workspace_slug": "acme",
  "workspace_name": "Acme",
  "generated_at": "2026-01-01T00:00:00Z",
  "generator_version": "static-v1",
  "projects": [
    {
      "slug": "solo",
      "root_path": "/r/solo",
      "git_remote": "",
      "summary": "",
      "summary_source": "",
      "capabilities": [],
      "capabilities_source": "",
      "depends_on": [
        "beta"
      ],
      "depends_on_source": "go.mod",
      "entry_points": [],
      "languages": {
        "go": 1
      },
      "planar_focus": {
        "active_plans": [
          3
        ],
        "open_tasks": 0,
        "open_questions": 0,
        "recent_session_ids": []
      }
    }
  ],
  "cross_repo": {
    "plans_scoped_to_org": [],
    "questions_scoped_to_org": [
      1
    ],
    "dependency_edges": [
      {
        "from": "solo",
        "to": "beta",
        "reason": "go.mod replace"
      }
    ]
  }
})";

  CHECK(routing::encode(table) == k_expected);
}

TEST_CASE("build then write then decode is a genuine round trip", "[engine][workspace][routing][build][roundtrip]") {
  // THE PROPERTY THIS CYCLE EXISTS FOR. Task 6110 shipped the decoder alone
  // and recorded that `build` then `show` was not yet a round trip here.
  // This closes it: build a table from a database, write the file, decode
  // the BYTES back, and compare field by field.
  //
  // The comparison goes through the file rather than through the in-memory
  // struct on purpose — an encoder that silently dropped a field would
  // otherwise be invisible.
  scratch_tree tree;
  auto         conn = open_migrated(tree);
  seed_fixture(tree, conn);
  exec(conn, "insert into plans (title, slug, status, scope_kind, scope_id) "
             "values ('direct', 'direct', 'active', 'repo', 1)");

  auto built = routing::build(conn, 1, "acme", "Acme", routing::default_capability_rules());
  REQUIRE(built.has_value());

  const auto path = tree.state() / "routing-table.json";
  REQUIRE(routing::write_table(path, *built));
  REQUIRE(std::filesystem::exists(path));

  std::ifstream     input(path, std::ios::binary);
  const std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  // The file the oracle writes ends in exactly one newline.
  REQUIRE(raw.size() > 1);
  CHECK(raw.back() == '\n');
  CHECK(raw[raw.size() - 2] == '}');

  auto decoded = routing::decode(raw);
  REQUIRE(decoded.has_value());

  CHECK(decoded->schema_version == built->schema_version);
  CHECK(decoded->workspace_id == built->workspace_id);
  CHECK(decoded->workspace_slug == built->workspace_slug);
  CHECK(decoded->workspace_name == built->workspace_name);
  CHECK(decoded->generated_at == built->generated_at);
  CHECK(decoded->generator_version == built->generator_version);
  REQUIRE(decoded->projects.size() == built->projects.size());
  for (std::size_t i = 0; i < built->projects.size(); ++i) {
    const auto& want = built->projects[i];
    const auto& got  = decoded->projects[i];
    INFO("project " << want.slug);
    CHECK(got.slug == want.slug);
    CHECK(got.root_path == want.root_path);
    CHECK(got.git_remote == want.git_remote);
    CHECK(got.summary == want.summary);
    CHECK(got.summary_source == want.summary_source);
    CHECK(got.capabilities == want.capabilities);
    CHECK(got.capabilities_source == want.capabilities_source);
    CHECK(got.depends_on == want.depends_on);
    CHECK(got.depends_on_source == want.depends_on_source);
    CHECK(got.entry_points == want.entry_points);
    CHECK(got.languages == want.languages);
    CHECK(got.focus.active_plans == want.focus.active_plans);
    CHECK(got.focus.open_tasks == want.focus.open_tasks);
    CHECK(got.focus.open_questions == want.focus.open_questions);
    CHECK(got.focus.recent_session_ids == want.focus.recent_session_ids);
  }
  CHECK(decoded->cross.plans_scoped_to_org == built->cross.plans_scoped_to_org);
  CHECK(decoded->cross.questions_scoped_to_org == built->cross.questions_scoped_to_org);
  REQUIRE(decoded->cross.dependency_edges.size() == built->cross.dependency_edges.size());
  for (std::size_t i = 0; i < built->cross.dependency_edges.size(); ++i) {
    CHECK(decoded->cross.dependency_edges[i].from == built->cross.dependency_edges[i].from);
    CHECK(decoded->cross.dependency_edges[i].to == built->cross.dependency_edges[i].to);
    CHECK(decoded->cross.dependency_edges[i].reason == built->cross.dependency_edges[i].reason);
  }

  // And the round trip is not trivially true because the table is empty.
  CHECK(decoded->projects.size() == 4);
  CHECK_FALSE(decoded->cross.dependency_edges.empty());
  CHECK_FALSE(project_named(*decoded, "alpha").summary.empty());
}

TEST_CASE("write_table replaces an existing file atomically and leaves no temp behind",
          "[engine][workspace][routing][build][write]") {
  scratch_tree tree;
  const auto   path = tree.state() / "routing-table.json";
  put(path, "PREVIOUS CONTENTS\n");

  const routing::routing_table table{.schema_version    = 1,
                                     .workspace_id      = 2,
                                     .workspace_slug    = "s",
                                     .workspace_name    = "n",
                                     .generated_at      = "T",
                                     .generator_version = "static-v1",
                                     .projects          = {},
                                     .cross             = {}};
  REQUIRE(routing::write_table(path, table));

  std::ifstream     input(path, std::ios::binary);
  const std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  CHECK_FALSE(raw.contains("PREVIOUS"));
  CHECK(raw.contains("\"workspace_slug\": \"s\""));
  // The `.tmp` sibling was renamed, not left lying beside the result.
  CHECK_FALSE(std::filesystem::exists(tree.state() / "routing-table.json.tmp"));
}
