// @file render.t.cpp
// @brief Unit tests for `planar.engine.ingest.render` (plan 996, task 6035).
//
// The JSON cases pin the WHOLE output string, not a substring. That is
// deliberate: this object is the M9 parity gate, so it must match the Zig
// implementation byte for byte — and a substring assertion would pass happily
// while the indent, a separator, or the blank line an empty `entities` array
// leaves behind had drifted. Anything that changes these literals is a
// contract change, not a formatting preference.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.ingest.render;
import planar.engine.ingest.diff;
import planar.engine.ingest.parse;

namespace {

namespace diff   = planar::engine::ingest::diff;
namespace render = planar::engine::ingest::render;
namespace parse  = planar::engine::ingest::parse;

/// @brief A diff with a child plan, one added and one updated task, and two
/// scenarios — one covering a task, one covering nothing.
[[nodiscard]] auto populated_diff() -> diff::diff {
  diff::diff d{};
  d.anchor_plan_id_ = 7;
  d.anchor_slug_    = "anc";
  d.assoc_slug_     = "wsx";
  d.current_status_ = "draft";

  diff::plan_entry plan{.op_ = diff::op::add, .title_ = "M1", .existing_id_ = 0, .tasks_ = {}};
  plan.tasks_.push_back({.op_               = diff::op::add,
                         .title_            = "Do a",
                         .body_             = "",
                         .touches_          = {"repo-x"},
                         .depends_          = {},
                         .slug_             = "do-a",
                         .next_action_      = "",
                         .existing_id_      = 0,
                         .child_plan_title_ = "M1"});
  plan.tasks_.push_back({.op_               = diff::op::update,
                         .title_            = "Do b",
                         .body_             = "",
                         .touches_          = {},
                         .depends_          = {},
                         .slug_             = "do-b",
                         .next_action_      = "",
                         .existing_id_      = 11,
                         .child_plan_title_ = "M1"});
  d.child_plans_.push_back(std::move(plan));

  d.scenarios_.push_back({.op_          = diff::op::add,
                          .title_       = "S1",
                          .body_        = "",
                          .kind_        = "unit",
                          .acceptance_  = "",
                          .verifies_    = {parse::task_ref{.kind_ = "task", .id_ = 0, .slug_ = "do-a"}},
                          .existing_id_ = 0});
  d.scenarios_.push_back({.op_          = diff::op::add,
                          .title_       = "Orphan",
                          .body_        = "",
                          .kind_        = "unit",
                          .acceptance_  = "",
                          .verifies_    = {},
                          .existing_id_ = 0});
  return d;
}

} // namespace

TEST_CASE("render_json emits the exact empty-diff object", "[ingest][render][parity]") {
  diff::diff d{};
  d.anchor_plan_id_ = 42;
  d.anchor_slug_    = "anchor";
  d.current_status_ = "draft";

  // Note the blank line inside `entities` — an empty array still emits the
  // opening newline and the closing `\n  ],`. The Zig original does the same,
  // and "tidying" it away would break the parity gate.
  constexpr std::string_view expected = R"({
  "anchor_plan_id": 42,
  "assoc_slug": "",
  "anchor_slug": "anchor",
  "entities": [

  ],
  "summary": {
    "additions": 0,
    "updates": 0,
    "removals": 0
  },
  "coverage": {
    "total_tasks": 0,
    "tasks_with_slug": 0,
    "tasks_without_slug": 0,
    "uncovered_task_slugs": [],
    "orphan_scenarios": []
  },
  "slug_collisions": []
}
)";
  CHECK(render::render_json(d) == expected);
}

TEST_CASE("render_json emits the exact populated-diff object", "[ingest][render][parity]") {
  const auto d = populated_diff();

  constexpr std::string_view expected = R"({
  "anchor_plan_id": 7,
  "assoc_slug": "wsx",
  "anchor_slug": "anc",
  "entities": [
    {"op": "add", "kind": "plan", "title": "M1", "scope": "assoc:wsx", "derives_from": "plan:7"},
    {"op": "add", "kind": "task", "title": "Do a", "scope": "assoc:wsx", "derives_from": "plan:M1", "touches": ["repo-x"]},
    {"op": "update", "kind": "task", "title": "Do b", "scope": "assoc:wsx", "derives_from": "plan:M1"}
  ],
  "summary": {
    "additions": 2,
    "updates": 1,
    "removals": 0
  },
  "coverage": {
    "total_tasks": 2,
    "tasks_with_slug": 2,
    "tasks_without_slug": 0,
    "uncovered_task_slugs": ["do-b"],
    "orphan_scenarios": ["Orphan"]
  },
  "slug_collisions": []
}
)";
  CHECK(render::render_json(d) == expected);
}

TEST_CASE("render_json escapes strings the way the parity oracle does", "[ingest][render][parity]") {
  diff::diff d{};
  d.anchor_plan_id_ = 1;
  d.anchor_slug_    = "a\"b\\c\nd\te";
  const auto out    = render::render_json(d);

  // The two mandatory escapes plus the short forms. `/` and non-ASCII bytes
  // are deliberately NOT escaped — escaping either would still be valid JSON
  // but not byte-identical, which is exactly what the gate compares.
  CHECK(out.contains(R"("anchor_slug": "a\"b\\c\nd\te")"));
}

TEST_CASE("render_json emits slug collisions with their holders", "[ingest][render]") {
  diff::diff d{};
  d.anchor_plan_id_ = 3;
  d.anchor_slug_    = "anc";
  d.slug_collisions_.push_back({.slug_ = "taken", .existing_task_id_ = 900, .existing_plan_id_ = 12});
  d.slug_collisions_.push_back({.slug_ = "also", .existing_task_id_ = 901, .existing_plan_id_ = 13});

  const auto out = render::render_json(d);
  CHECK(out.contains(
      R"("slug_collisions": [{"slug": "taken", "existing_task_id": 900, "existing_plan_id": 12}, {"slug": "also", "existing_task_id": 901, "existing_plan_id": 13}])"));
}

TEST_CASE("render_text prints Nothing to do. for an empty diff", "[ingest][render]") {
  diff::diff d{};
  d.anchor_plan_id_ = 7;
  d.anchor_slug_    = "anchor";
  d.current_status_ = "draft";

  const auto out = render::render_text(d, /*applied=*/false);
  CHECK(out.starts_with("plan:7/anchor/\n"));
  CHECK(out.contains("0 additions, 0 updates, 0 proposed removals.\n"));
  CHECK(out.contains("coverage: 0 tasks (0 with slug, 0 without)\n"));
  CHECK(out.contains("Nothing to do.\n"));
}

TEST_CASE("render_text reports coverage gaps and the apply reminder", "[ingest][render]") {
  const auto out = render::render_text(populated_diff(), /*applied=*/false);

  CHECK(out.starts_with("wsx/anc/\n"));
  CHECK(out.contains("2 additions, 1 updates, 0 proposed removals.\n"));
  CHECK(out.contains("coverage: 2 tasks (2 with slug, 0 without); 1 uncovered: do-b; 1 orphan scenarios: Orphan\n"));
  CHECK(out.contains("touches=repo-x"));
  CHECK(out.contains("Run with --apply to commit; add --apply-removals to cancel proposed removals.\n"));
}

TEST_CASE("render_text suppresses the footer when the diff was applied", "[ingest][render]") {
  const auto out = render::render_text(populated_diff(), /*applied=*/true);
  CHECK_FALSE(out.contains("Run with --apply"));
  CHECK_FALSE(out.contains("Nothing to do."));
  // The coverage line is still emitted, because scripts read it unconditionally.
  CHECK(out.contains("coverage: 2 tasks"));
}

TEST_CASE("render_text names every slug-collision holder", "[ingest][render]") {
  auto d = populated_diff();
  d.slug_collisions_.push_back({.slug_ = "do-a", .existing_task_id_ = 4242, .existing_plan_id_ = 99});

  const auto out = render::render_text(d, /*applied=*/false);
  CHECK(out.contains("slug-collisions: 1 task slug(s) already exist globally:\n"));
  CHECK(out.contains("conflict: slug 'do-a' already held by task 4242 (plan 99)"));
}

TEST_CASE("render_text truncates over-long titles to the display width", "[ingest][render]") {
  diff::diff d{};
  d.anchor_plan_id_ = 1;
  d.anchor_slug_    = "a";
  d.child_plans_.push_back({.op_ = diff::op::add, .title_ = std::string(50, 'x'), .existing_id_ = 0, .tasks_ = {}});

  const auto out = render::render_text(d, /*applied=*/false);
  CHECK(out.contains(std::string(33, 'x') + "..."));
  CHECK_FALSE(out.contains(std::string(37, 'x')));
}
