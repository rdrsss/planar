// @file render.t.cpp
// @brief Unit tests for `planar.engine.workbench.render` (plan 996, task
// 6037): the exact file bytes, the quoting rule, and the round trip.
//
// ORACLE PROVENANCE. Every expected payload below is a real file a real
// `workbench push` wrote to a scratch root, read back with `sed -n l` so the
// trailing double-space hard breaks are visible:
//
//   $Z workbench push 1
//   sed -n l <root>/project_demo/p1-demo-feature/1-tech-spec-auth.md
//     ---$
//     entity_kind: artifact$
//     entity_id: 1$
//     anchor_plan_id: 1$
//     title: 'Tech Spec: Auth'$
//     status: draft$
//     artifact_kind: tech_spec$
//     ---$
//     $
//     # Artifact 1: Tech Spec: Auth$
//     ...
//
// The whole eight-file tree was then diffed byte-for-byte between the two
// binaries: identical.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.parse;
import planar.engine.workbench.render;

namespace {

namespace wp = planar::engine::workbench::parse;
namespace wr = planar::engine::workbench::render;

} // namespace

TEST_CASE("the three identity fields are emitted unconditionally", "[workbench][render]") {
  // Including a ZERO anchor_plan_id: the prefix is invariant, and a renderer
  // that treated 0 as "omit" would produce a file the parser then rejects
  // for a different reason entirely.
  CHECK(wr::render(wp::front_matter{.entity_kind = "plan", .entity_id = 1}, "") ==
        "---\nentity_kind: plan\nentity_id: 1\nanchor_plan_id: 0\n---\n");
}

TEST_CASE("optional fields are emitted only when set, in declaration order", "[workbench][render]") {
  wp::front_matter fm{.entity_kind    = "artifact",
                      .entity_id      = 1,
                      .anchor_plan_id = 1,
                      .title          = "Tech Spec: Auth",
                      .status         = "draft",
                      .artifact_kind  = "tech_spec"};
  CHECK(wr::render(fm, "") == "---\n"
                              "entity_kind: artifact\n"
                              "entity_id: 1\n"
                              "anchor_plan_id: 1\n"
                              "title: 'Tech Spec: Auth'\n"
                              "status: draft\n"
                              "artifact_kind: tech_spec\n"
                              "---\n");
}

TEST_CASE("a zero priority is omitted and a non-zero one is emitted", "[workbench][render]") {
  wp::front_matter fm{.entity_kind = "task", .entity_id = 1, .anchor_plan_id = 1, .title = "T", .status = "todo"};
  CHECK(wr::render(fm, "").find("priority") == std::string::npos);
  fm.priority = 100;
  CHECK(wr::render(fm, "").find("priority: 100\n") != std::string::npos);
}

TEST_CASE("a non-empty body is preceded by exactly one blank line", "[workbench][render]") {
  wp::front_matter fm{.entity_kind = "task", .entity_id = 5, .anchor_plan_id = 2, .title = "T", .status = "todo"};
  auto const       out = wr::render(fm, "# Task 5: T\n");
  CHECK(out.ends_with("---\n\n# Task 5: T\n"));
}

TEST_CASE("an empty body appends NOTHING after the closing delimiter", "[workbench][render]") {
  wp::front_matter fm{.entity_kind = "task", .entity_id = 5, .anchor_plan_id = 2, .title = "T", .status = "todo"};
  CHECK(wr::render(fm, "").ends_with("---\n"));
  CHECK_FALSE(wr::render(fm, "").ends_with("---\n\n"));
}

TEST_CASE("lists render as block sequences, and refs as <kind>:<id>", "[workbench][render]") {
  wp::front_matter fm{.entity_kind    = "task",
                      .entity_id      = 5,
                      .anchor_plan_id = 2,
                      .title          = "T",
                      .status         = "todo",
                      .touches        = {"repo-a", "repo-b"},
                      .verifies       = {wp::entity_ref{.kind = "task", .id = 99}},
                      .cites          = {wp::entity_ref{.kind = "artifact", .id = 12}},
                      .derives_from   = {wp::entity_ref{.kind = "plan", .id = 3}}};
  auto const       out = wr::render(fm, "");
  CHECK(out.find("touches:\n- repo-a\n- repo-b\n") != std::string::npos);
  CHECK(out.find("verifies:\n- task:99\n") != std::string::npos);
  CHECK(out.find("cites:\n- artifact:12\n") != std::string::npos);
  // The DASH spelling, not `derives_from`. The parser treats the underscore
  // form as an unknown key and then refuses its items outright, so emitting
  // it would make this renderer's own output unparseable.
  CHECK(out.find("derives-from:\n- plan:3\n") != std::string::npos);
}

// --- quoting --------------------------------------------------------------

TEST_CASE("needs_yaml_quote covers the YAML indicators and the ': ' case", "[workbench][render][quote]") {
  CHECK(wr::needs_yaml_quote(""));
  CHECK(wr::needs_yaml_quote("Tech Spec: Auth"));
  CHECK(wr::needs_yaml_quote("Note:"));
  CHECK(wr::needs_yaml_quote("- item"));
  for (auto const* indicator :
       {"{a", "}a", "[a", "]a", ",a", "#a", "&a", "*a", "!a", "|a", ">a", "'a", "\"a", "%a", "@a", "`a"}) {
    CHECK(wr::needs_yaml_quote(indicator));
  }
  CHECK_FALSE(wr::needs_yaml_quote("plain"));
  CHECK_FALSE(wr::needs_yaml_quote("Ratio 3:4"));
  // A bare `-` that is NOT followed by a space is fine -- `-5` and
  // `-hyphen-slug` both render unquoted.
  CHECK_FALSE(wr::needs_yaml_quote("-5"));
}

TEST_CASE("the renderer quotes a trailing-colon value the PARSER would accept unquoted", "[workbench][render][quote]") {
  // The deliberate asymmetry: a hand-written `title: Note:` parses, and the
  // next push normalizes it to `title: 'Note:'`. Both halves oracle-probed;
  // pinned here so neither side is "made consistent" with the other.
  wp::front_matter fm{.entity_kind = "task", .entity_id = 1, .anchor_plan_id = 1, .title = "Note:", .status = "todo"};
  CHECK(wr::render(fm, "").find("title: 'Note:'\n") != std::string::npos);
  CHECK(wp::diagnose("---\nentity_kind: task\nentity_id: 1\ntitle: Note:\nstatus: todo\n---\n") == std::nullopt);
}

// --- round trip -----------------------------------------------------------

TEST_CASE("render then parse reconstructs the front matter and body", "[workbench][render][roundtrip]") {
  // The invariant that makes field ORDER load-bearing. If a future change
  // reorders or drops a field, this is what refuses it.
  wp::front_matter const fm{.entity_kind    = "artifact",
                            .entity_id      = 11,
                            .anchor_plan_id = 4,
                            .title          = "Tech Spec: Auth",
                            .status         = "active",
                            .priority       = 50,
                            .scope          = "repo:demo",
                            .artifact_kind  = "test_spec",
                            .touches        = {"repo-a"},
                            .verifies       = {wp::entity_ref{.kind = "task", .id = 99}},
                            .cites          = {wp::entity_ref{.kind = "artifact", .id = 12}},
                            .derives_from   = {wp::entity_ref{.kind = "plan", .id = 3}}};
  std::string const      content = wr::render(fm, "# Heading\n\nbody text\n");
  auto const             parsed  = wp::parse(content);
  REQUIRE(parsed.has_value());
  CHECK(parsed->frontmatter == fm);
  CHECK(parsed->body == "# Heading\n\nbody text\n");
}

TEST_CASE("a title needing quotes survives the round trip unquoted", "[workbench][render][roundtrip]") {
  for (auto const* title : {"Tech Spec: Auth", "Note:", "- leading dash", "#hash", "", "plain"}) {
    wp::front_matter  fm{.entity_kind = "task", .entity_id = 1, .anchor_plan_id = 1, .title = title, .status = "todo"};
    std::string const content = wr::render(fm, "");
    auto const        parsed  = wp::parse(content);
    if (std::string_view{title}.empty()) {
      // An empty title is a MISSING required field, so this one is expected
      // to be refused -- the renderer emits `title: ''` and the parser reads
      // the empty value as absent. Named rather than silently skipped.
      REQUIRE_FALSE(parsed.has_value());
      CHECK(parsed.error() == wp::parse_error_kind::missing_required_field);
      continue;
    }
    REQUIRE(parsed.has_value());
    CHECK(parsed->frontmatter.title == title);
  }
}

// --- task 6880: a front-matter value whose byte length is a multiple of 256
//
// The same libc++ `format_to(back_inserter)` hazard as the entity bodies
// (llvm/llvm-project#154670). The prefix (`title: `) is irrelevant: the
// formatter flushes before copying an argument that does not fit, so the
// boundary is the argument itself filling the 256-byte stack buffer exactly,
// with a literal (`\n`, or `'\n` for the quoted form) written after it.
TEST_CASE("a 256-byte title renders unquoted, and a 256-byte one renders quoted", "[workbench][render][6880]") {
  std::string const plain(256, 'p');
  CHECK(wr::render(wp::front_matter{.entity_kind = "plan", .entity_id = 1, .title = plain}, "") ==
        "---\nentity_kind: plan\nentity_id: 1\nanchor_plan_id: 0\ntitle: " + plain + "\n---\n");

  std::string const quoted = "Quoted: " + std::string(248, 'q');
  REQUIRE(quoted.size() == 256);
  CHECK(wr::render(wp::front_matter{.entity_kind = "plan", .entity_id = 1, .title = quoted}, "") ==
        "---\nentity_kind: plan\nentity_id: 1\nanchor_plan_id: 0\ntitle: '" + quoted + "'\n---\n");

  // A touches slug on the boundary goes through the list renderer.
  std::string const slug(512, 's');
  CHECK(wr::render(wp::front_matter{.entity_kind = "task", .entity_id = 1, .touches = {slug}}, "")
            .find("touches:\n- " + slug + "\n") != std::string::npos);
}
