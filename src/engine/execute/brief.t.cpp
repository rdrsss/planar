// @file brief.t.cpp
// @brief `compile_brief` over inputs whose rendered lines land on the
// formatter's 256-byte boundary (task 6880).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;

namespace brief = planar::engine::execute::brief;

// The pinned libc++ (23.1.1) writes one byte past its 256-byte stack buffer
// when `std::format_to(std::back_inserter(s), ...)` copies an argument that
// fills the buffer exactly and a literal follows (llvm/llvm-project#154670).
// The prefix is irrelevant (the formatter flushes before an argument that
// does not fit), so the boundary is an argument of exactly 256 bytes -- or
// any multiple -- with a literal written after it. Before the fix each case
// aborts the test process with `__stack_chk_fail`.
TEST_CASE("a context record body of exactly 256 bytes compiles", "[engine][execute][brief][6880]") {
  brief::brief_inputs inputs;
  inputs.plan        = {.id = 1, .title = "Demo", .status = "active"};
  inputs.tasks       = {{.id = 7, .plan_id = 1, .title = "Do it", .status = "doing"}};
  inputs.claim_token = "tok";
  std::string const boundary(256, 'r');
  inputs.context_records = {{.kind = "k", .body = boundary}};

  auto const out = brief::compile_brief(inputs);
  REQUIRE(out.has_value());
  CHECK(out->find("- **k**: " + boundary + "\n") != std::string::npos);
}

TEST_CASE("a problem statement and citation on the boundary compile", "[engine][execute][brief][6880]") {
  brief::brief_inputs inputs;
  inputs.plan        = {.id = 1, .title = "Demo", .status = "active"};
  inputs.tasks       = {{.id = 7, .plan_id = 1, .title = "Do it", .status = "doing"}};
  inputs.claim_token = "tok";
  std::string const path(256, 'c');
  inputs.spec_citations   = {{.path = path}};
  inputs.locked_decisions = {{.id = "1", .text = std::string(512, 'd')}};

  auto const out = brief::compile_brief(inputs);
  REQUIRE(out.has_value());
  CHECK(out->find("- `" + path + "`\n") != std::string::npos);
}
