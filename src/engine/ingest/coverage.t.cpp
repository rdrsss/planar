// @file coverage.t.cpp
// @brief Unit tests for `planar.engine.ingest.coverage` (plan 996, task 6035).
//
// The `--strict` gate reads this object and the M9 parity gate compares its
// JSON projection byte for byte, so the two ordering rules below (dedupe +
// sort for uncovered slugs; sort but NOT dedupe for orphan scenarios) are
// asserted directly rather than assumed.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.ingest.coverage;
import planar.engine.ingest.diff;
import planar.engine.ingest.parse;

namespace {

namespace coverage = planar::engine::ingest::coverage;
namespace diff     = planar::engine::ingest::diff;
namespace parse    = planar::engine::ingest::parse;

/// @brief Builds a task entry with just the fields coverage reads.
[[nodiscard]] auto task_of(diff::op operation, std::string_view title, std::string_view slug) -> diff::task_entry {
  return {.op_               = operation,
          .title_            = std::string{title},
          .body_             = "",
          .touches_          = {},
          .depends_          = {},
          .slug_             = std::string{slug},
          .next_action_      = "",
          .existing_id_      = 0,
          .child_plan_title_ = "M1"};
}

/// @brief Builds a scenario entry citing the given slugs.
[[nodiscard]] auto scenario_of(std::string_view title, std::span<const std::string_view> slugs) -> diff::scenario_entry {
  diff::scenario_entry entry{.op_          = diff::op::add,
                             .title_       = std::string{title},
                             .body_        = "",
                             .kind_        = "unit",
                             .acceptance_  = "",
                             .verifies_    = {},
                             .existing_id_ = 0};
  for (const auto slug : slugs) {
    entry.verifies_.push_back({.kind_ = "task", .id_ = 0, .slug_ = std::string{slug}});
  }
  return entry;
}

} // namespace

TEST_CASE("has_gaps is false for an empty coverage object", "[ingest][coverage]") {
  CHECK_FALSE(coverage::coverage{}.has_gaps());
}

TEST_CASE("has_gaps fires on an uncovered task and on an orphan scenario", "[ingest][coverage]") {
  coverage::coverage uncovered{};
  uncovered.uncovered_task_slugs_ = {"foo"};
  CHECK(uncovered.has_gaps());

  coverage::coverage orphaned{};
  orphaned.orphan_scenarios_ = {"S"};
  CHECK(orphaned.has_gaps());
}

TEST_CASE("compute counts tasks with and without a slug", "[ingest][coverage]") {
  diff::diff       d{};
  diff::plan_entry plan{.op_ = diff::op::add, .title_ = "M1", .existing_id_ = 0, .tasks_ = {}};
  plan.tasks_.push_back(task_of(diff::op::add, "a", "task-a"));
  plan.tasks_.push_back(task_of(diff::op::add, "b", ""));
  d.child_plans_.push_back(std::move(plan));

  const auto cov = coverage::compute(d);
  CHECK(cov.total_tasks_ == 2);
  CHECK(cov.tasks_with_slug_ == 1);
  CHECK(cov.tasks_without_slug_ == 1);
  REQUIRE(cov.uncovered_task_slugs_.size() == 1);
  CHECK(cov.uncovered_task_slugs_[0] == "task-a");
}

TEST_CASE("compute treats a slug cited by any scenario as covered", "[ingest][coverage]") {
  diff::diff       d{};
  diff::plan_entry plan{.op_ = diff::op::add, .title_ = "M1", .existing_id_ = 0, .tasks_ = {}};
  plan.tasks_.push_back(task_of(diff::op::add, "a", "task-a"));
  plan.tasks_.push_back(task_of(diff::op::update, "b", "task-b"));
  d.child_plans_.push_back(std::move(plan));

  const std::array<std::string_view, 1> cites_a{"task-a"};
  d.scenarios_.push_back(scenario_of("covers a", cites_a));

  const auto cov = coverage::compute(d);
  CHECK(cov.total_tasks_ == 2);
  REQUIRE(cov.uncovered_task_slugs_.size() == 1);
  CHECK(cov.uncovered_task_slugs_[0] == "task-b");
  CHECK(cov.orphan_scenarios_.empty());
  CHECK(cov.has_gaps());
}

TEST_CASE("compute ignores numeric refs for coverage", "[ingest][coverage]") {
  // A numeric ref cannot be tied to a roadmap bullet at preview time, so it
  // must not silently mark a slugged task as covered.
  diff::diff       d{};
  diff::plan_entry plan{.op_ = diff::op::add, .title_ = "M1", .existing_id_ = 0, .tasks_ = {}};
  plan.tasks_.push_back(task_of(diff::op::add, "a", "task-a"));
  d.child_plans_.push_back(std::move(plan));

  diff::scenario_entry numeric{.op_          = diff::op::add,
                               .title_       = "numeric only",
                               .body_        = "",
                               .kind_        = "unit",
                               .acceptance_  = "",
                               .verifies_    = {parse::task_ref{.kind_ = "task", .id_ = 99, .slug_ = ""}},
                               .existing_id_ = 0};
  d.scenarios_.push_back(std::move(numeric));

  const auto cov = coverage::compute(d);
  REQUIRE(cov.uncovered_task_slugs_.size() == 1);
  CHECK(cov.uncovered_task_slugs_[0] == "task-a");
  // A numeric ref is still a citation, so the scenario is not an orphan.
  CHECK(cov.orphan_scenarios_.empty());
}

TEST_CASE("compute reports a scenario citing nothing as an orphan", "[ingest][coverage]") {
  diff::diff                            d{};
  const std::array<std::string_view, 0> none{};
  d.scenarios_.push_back(scenario_of("cites nothing", none));

  const auto cov = coverage::compute(d);
  REQUIRE(cov.orphan_scenarios_.size() == 1);
  CHECK(cov.orphan_scenarios_[0] == "cites nothing");
  CHECK(cov.has_gaps());
}

TEST_CASE("compute sorts uncovered slugs bytewise", "[ingest][coverage]") {
  diff::diff       d{};
  diff::plan_entry plan{.op_ = diff::op::add, .title_ = "M1", .existing_id_ = 0, .tasks_ = {}};
  for (const auto slug : {"zeta", "alpha", "Mid", "mid"}) {
    plan.tasks_.push_back(task_of(diff::op::add, slug, slug));
  }
  d.child_plans_.push_back(std::move(plan));

  const auto cov = coverage::compute(d);
  // Bytewise, so uppercase sorts before lowercase — this is the ordering the
  // JSON projection is pinned to, not a locale-aware one.
  REQUIRE(cov.uncovered_task_slugs_.size() == 4);
  CHECK(cov.uncovered_task_slugs_[0] == "Mid");
  CHECK(cov.uncovered_task_slugs_[1] == "alpha");
  CHECK(cov.uncovered_task_slugs_[2] == "mid");
  CHECK(cov.uncovered_task_slugs_[3] == "zeta");
}

TEST_CASE("compute sorts orphan scenarios but does NOT deduplicate them", "[ingest][coverage]") {
  // Two distinct scenarios may legitimately share a title. Collapsing them
  // would under-report the very gap the strict gate exists to surface.
  diff::diff                            d{};
  const std::array<std::string_view, 0> none{};
  d.scenarios_.push_back(scenario_of("same title", none));
  d.scenarios_.push_back(scenario_of("another", none));
  d.scenarios_.push_back(scenario_of("same title", none));

  const auto cov = coverage::compute(d);
  REQUIRE(cov.orphan_scenarios_.size() == 3);
  CHECK(cov.orphan_scenarios_[0] == "another");
  CHECK(cov.orphan_scenarios_[1] == "same title");
  CHECK(cov.orphan_scenarios_[2] == "same title");
}

TEST_CASE("compute skips tasks proposed for removal", "[ingest][coverage]") {
  diff::diff       d{};
  diff::plan_entry plan{.op_ = diff::op::add, .title_ = "M1", .existing_id_ = 0, .tasks_ = {}};
  plan.tasks_.push_back(task_of(diff::op::add, "kept", "kept"));
  plan.tasks_.push_back(task_of(diff::op::remove, "gone", "gone"));
  d.child_plans_.push_back(std::move(plan));

  const auto cov = coverage::compute(d);
  CHECK(cov.total_tasks_ == 1);
  REQUIRE(cov.uncovered_task_slugs_.size() == 1);
  CHECK(cov.uncovered_task_slugs_[0] == "kept");
}
