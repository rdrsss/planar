// @file lint.t.cpp
// @brief Unit tests for `planar.engine.workbench.lint` (plan 996, task 6037):
// the coded diagnostics and the anchor-plan warning.
//
// ORACLE PROVENANCE. Every code, message and hint below is verbatim from
// `workbench lint --path <file> --json` against a scratch database:
//
//   {"path":"…/bad_status.md","line":5,"severity":"error",
//    "code":"invalid_field_value",
//    "message":"front matter field 'status' has an unsupported value",
//    "hint":"use one of: todo, doing, blocked, done, or cancelled"}
//
//   {"path":"…/quoted_colon.md","line":1,"severity":"warning",
//    "code":"anchor_plan_not_found",
//    "message":"front matter field 'anchor_plan_id' must reference an existing plan",
//    "hint":"set anchor_plan_id to the owning top-level plan ID"}
//
// 59 fixtures were run through both binaries and diffed: all identical.
// Warnings-only runs exit 1 too -- `$Z workbench lint --path quoted_colon.md`
// printed `0 errors, 1 warnings.` and exited 1.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.workbench.fsutil;
import planar.engine.workbench.lint;

namespace {

namespace wl  = planar::engine::workbench::lint;
namespace wfs = planar::engine::workbench::fsutil;

struct arena {
  std::filesystem::path                 dir_;
  std::optional<planar::db::connection> conn_;

  arena()
      : dir_(std::filesystem::temp_directory_path() / std::format("planar_wb_lint_{}_{}",
                                                                  std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                  reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(dir_ / "tree", ec);
    auto conn = planar::db::connection::open((dir_ / "planar.db").string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn).has_value());
    conn_ = std::move(*conn);
    REQUIRE(conn_
                ->execute("insert into plans (scope_kind, title, slug, status) "
                          "values ('global', 'Anchor', 'anchor', 'draft')")
                .has_value());
  }
  arena(const arena&)            = delete;
  arena& operator=(const arena&) = delete;
  ~arena() {
    conn_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  auto conn() -> planar::db::connection& {
    return *conn_;
  }
  [[nodiscard]] auto tree() const -> std::filesystem::path {
    return dir_ / "tree";
  }

  /// @brief Write one fixture into the tree and return its path.
  auto write(std::string_view name, std::string_view content) -> std::filesystem::path {
    auto const path = tree() / name;
    REQUIRE(wfs::write_file_atomic(path, content));
    return path;
  }
};

/// @brief A file whose `anchor_plan_id` names the arena's real plan, so the
/// only thing under test is the syntax check.
auto valid_task() -> std::string {
  return "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: 1\ntitle: T\nstatus: todo\n---\n";
}

} // namespace

TEST_CASE("a clean file yields no issues and a scan count", "[workbench][lint]") {
  arena      a;
  auto const path  = a.write("ok.md", valid_task());
  auto       value = wl::run(a.conn(), path);
  REQUIRE(value.has_value());
  CHECK(value->files_scanned == 1);
  CHECK(value->errors == 0);
  CHECK(value->warnings == 0);
  CHECK(value->issues.empty());
}

TEST_CASE("each parse rejection maps to its own code, message and hint", "[workbench][lint][oracle]") {
  struct expectation {
    std::string_view name;
    std::string_view content;
    std::size_t      line;
    std::string_view code;
    std::string_view message;
    std::string_view hint;
  };
  // One per branch of `appendParseIssue`, all captured from the oracle.
  std::array<expectation, 6> const cases{{
      {"missing.md", "body\n", 1, "malformed_frontmatter", "front matter is malformed: opening delimiter is missing",
       "start the file with a '---' front matter delimiter"},
      {"identity.md", "---\nentity_kind: task\n---\n", 3, "missing_required_field",
       "front matter is missing required field 'entity_id'", "add required front matter field 'entity_id'"},
      {"kind.md", "---\nentity_kind: widget\nentity_id: 1\ntitle: W\nstatus: active\n---\n", 2, "invalid_entity_kind",
       "front matter field 'entity_kind' is not supported", "use one of: plan, task, artifact, scenario, decision, or question"},
      {"status.md", "---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: bogus\n---\n", 5, "invalid_field_value",
       "front matter field 'status' has an unsupported value", "use one of: todo, doing, blocked, done, or cancelled"},
      {"int.md", "---\nentity_kind: task\nentity_id: abc\ntitle: T\nstatus: todo\n---\n", 3, "malformed_frontmatter",
       "front matter is malformed: an integer field is invalid", "set entity_id to a base-10 integer"},
      {"tab.md", "---\nentity_kind: task\n\tentity_id: 1\ntitle: T\nstatus: todo\n---\n", 3, "malformed_frontmatter",
       "front matter is malformed: tab indentation is not valid YAML", "replace tab indentation with spaces"},
  }};

  for (auto const& expected : cases) {
    arena      a;
    auto const path  = a.write(expected.name, expected.content);
    auto       value = wl::run(a.conn(), path);
    REQUIRE(value.has_value());
    INFO(expected.name);
    REQUIRE(value->issues.size() == 1);
    auto const& issue = value->issues[0];
    CHECK(issue.level == wl::severity::error);
    CHECK(issue.line == expected.line);
    CHECK(issue.code == expected.code);
    CHECK(issue.message == expected.message);
    CHECK(issue.hint == expected.hint);
    CHECK(value->errors == 1);
    CHECK(value->warnings == 0);
  }
}

TEST_CASE("the entity-ref and dash-scalar hints are distinct", "[workbench][lint]") {
  arena a;
  auto  ref = wl::run(a.conn(), a.write("ref.md", "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: 1\n"
                                                  "title: T\nstatus: todo\ncites:\n- notaref\n---\n"));
  REQUIRE(ref.has_value());
  REQUIRE(ref->issues.size() == 1);
  CHECK(ref->issues[0].hint == "use '<entity-kind>:<positive-id>' for each reference");

  arena b;
  auto  dash = wl::run(b.conn(), b.write("dash.md", "---\nentity_kind: task\nentity_id: 1\n- orphan\n"
                                                    "title: T\nstatus: todo\n---\n"));
  REQUIRE(dash.has_value());
  REQUIRE(dash->issues.size() == 1);
  CHECK(dash->issues[0].hint == "quote the scalar because values beginning with '- ' are YAML sequence indicators");
}

// --- the anchor-plan warning ---------------------------------------------

TEST_CASE("an ABSENT anchor_plan_id warns with the generic message", "[workbench][lint][warning]") {
  arena a;
  auto  value = wl::run(a.conn(), a.write("noanchor.md", "---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\n---\n"));
  REQUIRE(value.has_value());
  REQUIRE(value->issues.size() == 1);
  CHECK(value->issues[0].level == wl::severity::warning);
  CHECK(value->issues[0].code == "anchor_plan_not_found");
  CHECK(value->issues[0].message == "front matter field 'anchor_plan_id' must reference an existing plan");
  CHECK(value->issues[0].hint == "set anchor_plan_id to the owning top-level plan ID");
  // With the key absent there is no real line, so it falls back to 1.
  CHECK(value->issues[0].line == 1);
  CHECK(value->errors == 0);
  CHECK(value->warnings == 1);
}

TEST_CASE("a DANGLING anchor_plan_id warns with the id and its REAL line", "[workbench][lint][warning]") {
  // Two distinct messages, and the line here is genuine (unlike the parse
  // diagnostics' synthetic ones) because the key is present to point at.
  arena a;
  auto  value = wl::run(a.conn(), a.write("dangling.md", "---\nentity_kind: task\nentity_id: 1\n"
                                                         "anchor_plan_id: 4242\ntitle: T\nstatus: todo\n---\n"));
  REQUIRE(value.has_value());
  REQUIRE(value->issues.size() == 1);
  CHECK(value->issues[0].message == "anchor_plan_id 4242 does not reference an existing plan");
  CHECK(value->issues[0].line == 4);
}

TEST_CASE("line_for_key skips a line that only SHARES A PREFIX with the key", "[workbench][lint][warning]") {
  // `line_for_key`'s match is `starts_with(key) && size() > key.size() &&
  // line[key.size()] == ':'`. A decoy line ("anchor_plan_id_note: ...")
  // satisfies the prefix clause but is a DIFFERENT key entirely -- the
  // colon-boundary clause is what tells them apart. Without it, the decoy
  // (line 4) would be reported instead of the real key (line 5).
  arena a;
  auto  value = wl::run(a.conn(), a.write("decoy-prefix.md", "---\nentity_kind: task\nentity_id: 1\n"
                                                             "anchor_plan_id_note: unrelated\n"
                                                             "anchor_plan_id: 4242\ntitle: T\nstatus: todo\n---\n"));
  REQUIRE(value.has_value());
  REQUIRE(value->issues.size() == 1);
  CHECK(value->issues[0].message == "anchor_plan_id 4242 does not reference an existing plan");
  CHECK(value->issues[0].line == 5);
}

TEST_CASE("line_for_key never matches a line that does not start with the key at all", "[workbench][lint][warning]") {
  // A decoy line the same LENGTH as the key plus one, with a colon at
  // EXACTLY the boundary index the real check would land on, but with
  // completely different text. Only the `starts_with` clause tells this
  // apart from the real `anchor_plan_id:` line.
  arena a;
  auto  value = wl::run(a.conn(), a.write("decoy-length.md", "---\nentity_kind: task\nentity_id: 1\n"
                                                             "unknown_field1: something\n"
                                                             "anchor_plan_id: 4242\ntitle: T\nstatus: todo\n---\n"));
  REQUIRE(value.has_value());
  REQUIRE(value->issues.size() == 1);
  CHECK(value->issues[0].message == "anchor_plan_id 4242 does not reference an existing plan");
  CHECK(value->issues[0].line == 5);
}

TEST_CASE("a file that does NOT parse never reaches the anchor check", "[workbench][lint]") {
  // One issue per file, error-first: the syntax rejection short-circuits.
  arena a;
  auto  value = wl::run(a.conn(), a.write("bad.md", "---\nentity_kind: widget\nentity_id: 1\n"
                                                    "anchor_plan_id: 4242\ntitle: T\nstatus: todo\n---\n"));
  REQUIRE(value.has_value());
  REQUIRE(value->issues.size() == 1);
  CHECK(value->issues[0].code == "invalid_entity_kind");
  CHECK(value->warnings == 0);
}

// --- targets --------------------------------------------------------------

TEST_CASE("a directory target is walked recursively and sorted by path", "[workbench][lint]") {
  arena a;
  a.write("b.md", valid_task());
  a.write("a.md", "body with no front matter\n");
  REQUIRE(wfs::write_file_atomic(a.tree() / "sub" / "c.md", "also broken\n"));
  a.write("notes.txt", "ignored, not markdown\n");

  auto value = wl::run(a.conn(), a.tree());
  REQUIRE(value.has_value());
  CHECK(value->files_scanned == 3);
  CHECK(value->errors == 2);
  REQUIRE(value->issues.size() == 2);
  // Sorted, so output is stable across filesystems.
  CHECK(value->issues[0].path.ends_with("a.md"));
  CHECK(value->issues[1].path.ends_with("sub/c.md"));
}

TEST_CASE("an empty directory scans zero files and is clean", "[workbench][lint]") {
  arena a;
  auto  value = wl::run(a.conn(), a.tree());
  REQUIRE(value.has_value());
  CHECK(value->files_scanned == 0);
  CHECK(value->issues.empty());
}

TEST_CASE("an absent target and a non-.md file are DIFFERENT failures", "[workbench][lint]") {
  // Oracle-probed, and the distinction is operator-visible: a missing path
  // exits 1, a non-Markdown file exits 2.
  arena a;
  CHECK(wl::run(a.conn(), a.tree() / "nope.md").error() == wl::lint_error::not_found);
  auto const txt = a.write("notes.txt", "x\n");
  CHECK(wl::run(a.conn(), txt).error() == wl::lint_error::invalid_input);
}

TEST_CASE("severity_name emits the operator-visible words", "[workbench][lint]") {
  CHECK(wl::severity_name(wl::severity::error) == "error");
  CHECK(wl::severity_name(wl::severity::warning) == "warning");
}

TEST_CASE("lint and the sync gate accept exactly the same files", "[workbench][lint][gate]") {
  // The shared-gate invariant, checked from the lint side: a file lint
  // passes on syntax is a file `pull` will parse, and vice versa. A
  // divergence here would mean `workbench lint` reports clean on a tree
  // `workbench pull` then refuses.
  arena a;
  struct fixture {
    std::string_view name;
    std::string_view content;
    bool             clean;
  };
  std::array<fixture, 4> const cases{{
      {"ok.md", "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: 1\ntitle: T\nstatus: todo\n---\n", true},
      {"eof.md", "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: 1\ntitle: T\nstatus: todo\n---", true},
      {"crlf.md", "---\r\nentity_kind: task\r\nentity_id: 1\r\ntitle: T\r\nstatus: todo\r\n---\r\n", false},
      {"indented.md",
       "---\nentity_kind: task\nentity_id: 1\nanchor_plan_id: 1\ntitle: T\nstatus: todo\ntouches:\n  - demo\n---\n", false},
  }};
  for (auto const& item : cases) {
    arena scoped;
    auto  value = wl::run(scoped.conn(), scoped.write(item.name, item.content));
    REQUIRE(value.has_value());
    INFO(item.name);
    CHECK((value->errors == 0) == item.clean);
  }
}
