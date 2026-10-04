// models_role_value.t.cpp: `models resolve --role` keeps taking the bare role
// value after the agent role files were renamed `planar-<role>` (plan 1104,
// task 7209). A role value (`coder`) names a routing role; the agent file that
// does the work is `planar-coder`, and the two must not be confused.

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace parity = planar::cmd::parity;

namespace {

auto resolve(parity::arena const& arena, std::string_view tag, std::string_view role) -> parity::capture {
  std::vector<std::string> const args{"models", "resolve", "--role", std::string{role}};
  return parity::run_pinned(std::filesystem::path{PLANAR_CPP_BIN}, args, arena.cpp_root, tag);
}

} // namespace

TEST_CASE("models resolve accepts the bare role value and refuses the agent name", "[cmd][models][agents-contract]") {
  auto const arena = parity::make_arena("models-role-value");

  // The bare value is a known role: the refusal that follows is the missing --task, not an unknown role.
  auto const bare = resolve(arena, "bare", "coder");
  INFO("stdout:\n" << bare.out << "stderr:\n" << bare.err);
  CHECK(bare.err.find("unknown role") == std::string::npos);
  CHECK(bare.err.find("--task is required for task-bound role 'coder'") != std::string::npos);

  // The agent name is not a role.
  auto const prefixed = resolve(arena, "prefixed", "planar-coder");
  INFO("stdout:\n" << prefixed.out << "stderr:\n" << prefixed.err);
  CHECK(prefixed.err.find("unknown role 'planar-coder'") != std::string::npos);
}
