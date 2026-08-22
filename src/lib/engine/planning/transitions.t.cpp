// @file transitions.t.cpp
// @brief Unit tests for `planar.engine.planning.transitions` (plan 996,
// task cpp-planning-verbs). Pins the plan and task status-transition
// matrices against zig/src/engine/policy/status.zig's `.plan`/`.task`
// arms (captured by reading the Zig source, then cross-checked by
// running ./zig/zig-out/bin/planar against a scratch DB — see this
// task's coder report for the oracle transcript).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.planning.transitions;

using planar::engine::planning::check_transition;
using planar::engine::planning::transition_error;
using planar::engine::planning::transition_kind;

// --- plan arm ------------------------------------------------------------

TEST_CASE("plan arm: legal edges are accepted", "[transitions][plan]") {
  CHECK(check_transition(transition_kind::plan, "draft", "active", false).has_value());
  CHECK(check_transition(transition_kind::plan, "active", "paused", false).has_value());
  CHECK(check_transition(transition_kind::plan, "active", "done", false).has_value());
  CHECK(check_transition(transition_kind::plan, "active", "abandoned", false).has_value());
  CHECK(check_transition(transition_kind::plan, "paused", "active", false).has_value());
}

TEST_CASE("plan arm: skip-ahead moves are refused", "[transitions][plan]") {
  auto r1 = check_transition(transition_kind::plan, "draft", "done", false);
  REQUIRE_FALSE(r1.has_value());
  CHECK(r1.error() == transition_error::illegal_transition);
  CHECK_FALSE(check_transition(transition_kind::plan, "draft", "abandoned", false).has_value());
  CHECK_FALSE(check_transition(transition_kind::plan, "draft", "paused", false).has_value());
}

TEST_CASE("plan arm: terminal sources are refused", "[transitions][plan]") {
  CHECK_FALSE(check_transition(transition_kind::plan, "done", "draft", false).has_value());
  CHECK_FALSE(check_transition(transition_kind::plan, "done", "active", false).has_value());
  CHECK_FALSE(check_transition(transition_kind::plan, "abandoned", "active", false).has_value());
}

TEST_CASE("plan arm: identity transition is a no-op", "[transitions][plan]") {
  CHECK(check_transition(transition_kind::plan, "draft", "draft", false).has_value());
  CHECK(check_transition(transition_kind::plan, "done", "done", false).has_value());
  CHECK(check_transition(transition_kind::plan, "abandoned", "abandoned", false).has_value());
}

TEST_CASE("plan arm: unknown status returns unknown_status", "[transitions][plan]") {
  auto r = check_transition(transition_kind::plan, "cancelled", "active", false);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == transition_error::unknown_status);
}

// --- task arm --------------------------------------------------------------

TEST_CASE("task arm: legal edges among active states are accepted", "[transitions][task]") {
  CHECK(check_transition(transition_kind::task, "todo", "doing", false).has_value());
  CHECK(check_transition(transition_kind::task, "todo", "blocked", false).has_value());
  CHECK(check_transition(transition_kind::task, "todo", "cancelled", false).has_value());
  CHECK(check_transition(transition_kind::task, "doing", "todo", false).has_value());
  CHECK(check_transition(transition_kind::task, "doing", "blocked", false).has_value());
  CHECK(check_transition(transition_kind::task, "doing", "done", false).has_value());
  CHECK(check_transition(transition_kind::task, "doing", "cancelled", false).has_value());
  CHECK(check_transition(transition_kind::task, "blocked", "doing", false).has_value());
  CHECK(check_transition(transition_kind::task, "blocked", "done", false).has_value());
  CHECK(check_transition(transition_kind::task, "blocked", "cancelled", false).has_value());
}

TEST_CASE("task arm: terminal sources are refused by bare update", "[transitions][task]") {
  CHECK_FALSE(check_transition(transition_kind::task, "done", "todo", false).has_value());
  CHECK_FALSE(check_transition(transition_kind::task, "done", "doing", false).has_value());
  CHECK_FALSE(check_transition(transition_kind::task, "done", "blocked", false).has_value());
  CHECK_FALSE(check_transition(transition_kind::task, "cancelled", "todo", false).has_value());
}

TEST_CASE("task arm: illegal intermediate move (todo -> done, skipping doing) is refused", "[transitions][task]") {
  auto r = check_transition(transition_kind::task, "todo", "done", false);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == transition_error::illegal_transition);
}

TEST_CASE("task arm: identity transition is a no-op", "[transitions][task]") {
  CHECK(check_transition(transition_kind::task, "todo", "todo", false).has_value());
  CHECK(check_transition(transition_kind::task, "done", "done", false).has_value());
  CHECK(check_transition(transition_kind::task, "cancelled", "cancelled", false).has_value());
}

TEST_CASE("task arm: force=true bypasses the matrix for terminal sources", "[transitions][task]") {
  CHECK(check_transition(transition_kind::task, "done", "todo", true).has_value());
  CHECK(check_transition(transition_kind::task, "done", "doing", true).has_value());
  CHECK(check_transition(transition_kind::task, "cancelled", "blocked", true).has_value());
}

TEST_CASE("task arm: force=true also bypasses non-terminal illegal moves", "[transitions][task]") {
  CHECK(check_transition(transition_kind::task, "todo", "done", true).has_value());
}

TEST_CASE("task arm: unknown status returns unknown_status", "[transitions][task]") {
  auto r = check_transition(transition_kind::task, "inbox", "doing", false);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == transition_error::unknown_status);
}

// --- empty/absent cases ------------------------------------------------

TEST_CASE("empty status strings are unknown, not illegal", "[transitions][empty]") {
  auto r_plan = check_transition(transition_kind::plan, "", "active", false);
  REQUIRE_FALSE(r_plan.has_value());
  CHECK(r_plan.error() == transition_error::unknown_status);

  auto r_task = check_transition(transition_kind::task, "", "doing", false);
  REQUIRE_FALSE(r_task.has_value());
  CHECK(r_task.error() == transition_error::unknown_status);
}

TEST_CASE("empty-to-empty identity is still a no-op even though the status is unknown", "[transitions][empty]") {
  // Mirrors the Zig original's unconditional top-of-check identity
  // shortcut: `from == to` returns success BEFORE the per-kind switch
  // ever inspects whether the string is a recognized status.
  CHECK(check_transition(transition_kind::plan, "", "", false).has_value());
  CHECK(check_transition(transition_kind::task, "", "", false).has_value());
}
