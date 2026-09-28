// @file propagate.t.cpp
// @brief Unit tests for `planar.engine.extsync.propagate` (plan 996, task
// 6335).
//
// The function under test is a fallback, and the thing most worth pinning is
// what it does NOT do: it never consults a database and it never returns the
// zero-repo or projects-v2 bucket. A reader who assumes `github-issues`
// resolves by repo count is reading `selectStrategy`, which is a different
// (unported) function. See propagate.cppm.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.extsync.propagate;

namespace {

namespace prop = planar::engine::extsync::propagate;

} // namespace

TEST_CASE("strategy_for_system maps jira to the epic/story/sub-task triple", "[engine][extsync][propagate]") {
  auto const picked = prop::strategy_for_system("jira");
  REQUIRE(picked.has_value());
  CHECK(picked->kind == "jira-epic");
  CHECK(picked->plan_anchor_kind == "epic");
  CHECK(picked->plan_child_kind == "story");
  CHECK(picked->task_kind == "sub-task");
}

TEST_CASE("strategy_for_system ALWAYS returns parent-issue for github-issues", "[engine][extsync][propagate]") {
  auto const picked = prop::strategy_for_system("github-issues");
  REQUIRE(picked.has_value());
  // NOT `github-zero-repo` and NOT `github-projects-v2`, regardless of what
  // the local database contains — this function never opens one. Those two
  // buckets belong to `selectStrategy`, which is deferred with `ext
  // propagate`.
  CHECK(picked->kind == "github-parent-issue");
  CHECK(picked->plan_anchor_kind == "parent-issue");
  CHECK(picked->plan_child_kind == "issue");
  CHECK(picked->task_kind == "sub-task");
}

TEST_CASE("strategy_for_system refuses anything that is not one of the two kinds", "[engine][extsync][propagate]") {
  // The PRESENT cases above are what make these absences meaningful: a
  // function that returned nullopt unconditionally would satisfy this case
  // alone.
  CHECK_FALSE(prop::strategy_for_system("github-projects").has_value());
  CHECK_FALSE(prop::strategy_for_system("gitlab").has_value());
  CHECK_FALSE(prop::strategy_for_system("").has_value());
  // Exact and case-SENSITIVE, matching the oracle's `std::mem.eql`.
  CHECK_FALSE(prop::strategy_for_system("Jira").has_value());
  CHECK_FALSE(prop::strategy_for_system("JIRA").has_value());
  // A near miss on the accepted spelling, so the comparison cannot be a
  // prefix or substring match.
  CHECK_FALSE(prop::strategy_for_system("github-issue").has_value());
  CHECK_FALSE(prop::strategy_for_system("github-issues-v2").has_value());
  CHECK_FALSE(prop::strategy_for_system("jira-cloud").has_value());
}

TEST_CASE("a returned strategy outlives the call", "[engine][extsync][propagate]") {
  // Every field is a view into a string literal with static storage
  // duration, so copying the struct out of the expression that produced it
  // is safe. If any field were ever built into a temporary `std::string`,
  // this case would read freed memory rather than pass.
  prop::strategy held{};
  {
    auto const picked = prop::strategy_for_system("jira");
    REQUIRE(picked.has_value());
    held = *picked;
  }
  CHECK(held.kind == "jira-epic");
  CHECK(held.task_kind == "sub-task");
}

TEST_CASE("ADR-0006 buckets GitHub by distinct repo count while Jira ignores it", "[engine][extsync][propagate]") {
  CHECK(prop::strategy_for_repo_count("github-issues", 0)->kind == "github-zero-repo");
  CHECK(prop::strategy_for_repo_count("github-issues", 1)->kind == "github-parent-issue");
  CHECK(prop::strategy_for_repo_count("github-issues", 2)->kind == "github-projects-v2");
  CHECK(prop::strategy_for_repo_count("github-issues", 99)->kind == "github-projects-v2");
  CHECK(prop::strategy_for_repo_count("jira", 0)->kind == "jira-epic");
  CHECK(prop::strategy_for_repo_count("jira", 2)->kind == "jira-epic");
  CHECK_FALSE(prop::strategy_for_repo_count("gitlab", 2).has_value());
}
