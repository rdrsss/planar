// @file state.t.cpp
// @brief Pure JSON-decode fixtures for `planar::engine::execute::state` —
// the slice of `state.zig` ported at task 6125 for `ctx.brief`. No
// subprocess, no live `planar` binary; every fixture below mirrors a real
// shape captured by `state.zig`'s own fixture tests
// (`zig/src/cmd/planar-execute/state.zig`).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;

namespace {

namespace ex = planar::engine::execute;

} // namespace

// ---------------------------------------------------------------------------
// state::parse_plan_show
// ---------------------------------------------------------------------------

TEST_CASE("state::parse_plan_show decodes a real fixture; extras ignored", "[engine][execute][state]") {
  // Real JSON shape captured from `planar plan show 492 --json`.
  constexpr std::string_view fixture = R"JSON({"id":492,"scope_kind":"association","scope_id":1,
"title":"Autonomous workflow harness (planar-execute)","slug":"orchestrate-harness","summary":null,
"status":"active","parent_plan_id":null,"created_at":"2026-05-29T21:21:10.629Z",
"updated_at":"2026-06-03T00:15:44.069Z"})JSON";

  auto const got = ex::state::parse_plan_show(fixture);
  REQUIRE(got.has_value());
  CHECK(got->id == 492);
  CHECK(got->title == "Autonomous workflow harness (planar-execute)");
  CHECK(got->status == "active");
  REQUIRE(got->slug.has_value());
  CHECK(*got->slug == "orchestrate-harness");
  CHECK(!got->parent_plan_id.has_value());
}

TEST_CASE("state::parse_plan_show reports malformed on missing required fields", "[engine][execute][state]") {
  auto const missing_title = ex::state::parse_plan_show(R"JSON({"id":1,"status":"active"})JSON");
  REQUIRE(!missing_title.has_value());
  CHECK(missing_title.error() == ex::state::state_parse_error::malformed);

  auto const not_json = ex::state::parse_plan_show("{ not json {{{{");
  REQUIRE(!not_json.has_value());
}

// ---------------------------------------------------------------------------
// state::parse_task_show
// ---------------------------------------------------------------------------

TEST_CASE("state::parse_task_show decodes a blocked task with its slug (M7 block-detection shape)", "[engine][execute][state]") {
  constexpr std::string_view fixture =
      R"JSON({"id":3194,"plan_id":500,"title":"m7-local-block","body":"x",
"slug":"m7-local-block","status":"blocked","priority":100})JSON";

  auto const got = ex::state::parse_task_show(fixture);
  REQUIRE(got.has_value());
  CHECK(got->id == 3194);
  CHECK(got->status == "blocked");
  REQUIRE(got->slug.has_value());
  CHECK(*got->slug == "m7-local-block");
}

TEST_CASE("state::parse_task_show tolerates a null slug (defensive parsing against older rows)", "[engine][execute][state]") {
  auto const got = ex::state::parse_task_show(R"JSON({"id":1,"status":"todo","slug":null})JSON");
  REQUIRE(got.has_value());
  CHECK(!got->slug.has_value());
}

// ---------------------------------------------------------------------------
// state::parse_task_packet
// ---------------------------------------------------------------------------

namespace {

/// @brief One minimal, valid `packet_evidence` JSON row for fixture
/// composition below.
constexpr std::string_view k_evidence_row =
    R"JSON({"kind":"plan","id":1,"locator":"plan:1","text":"t","source_digest":"d","current_digest":"d",
"required":true,"covered":true,"status":"current","provenance":"db"})JSON";

} // namespace

TEST_CASE("state::parse_task_packet decodes a ready packet with typed evidence arrays", "[engine][execute][state]") {
  auto const fixture = std::format(
      R"JSON({{"input":{{"task_id":9,"status":"todo","title":"T","body":"B","next_action":"N","acceptance_criteria":"A",
"owning_plans":[{0}],"anchor_plans":[],"citations":[{0}],"decisions":[],"questions":[],"scenarios":[],
"dependencies":[],"touches":[],"claims":[{0}],"validation_gates":[],"facts":[]}}, "digest":"dg", "reasons":[]}})JSON",
      k_evidence_row);

  auto const got = ex::state::parse_task_packet(fixture);
  REQUIRE(got.has_value());
  CHECK(got->ready());
  CHECK(got->digest == "dg");
  CHECK(got->input.task_id == 9);
  REQUIRE(got->input.owning_plans.size() == 1);
  CHECK(got->input.owning_plans[0].id == 1);
  CHECK(got->input.owning_plans[0].required);
  CHECK(got->input.anchor_plans.empty());
  REQUIRE(got->input.claims.size() == 1);
  CHECK(got->input.claims[0].status == "current");
}

TEST_CASE("state::parse_task_packet: non-empty reasons means not ready", "[engine][execute][state]") {
  auto const fixture =
      R"JSON({"input":{"task_id":1,"status":"todo","title":"T","body":"B","next_action":"N","acceptance_criteria":"A",
"owning_plans":[],"anchor_plans":[],"citations":[],"decisions":[],"questions":[],"scenarios":[],
"dependencies":[],"touches":[],"claims":[],"validation_gates":[],"facts":[]}, "digest":"dg",
"reasons":["missing citation"]})JSON";

  auto const got = ex::state::parse_task_packet(fixture);
  REQUIRE(got.has_value());
  CHECK(!got->ready());
}

TEST_CASE("state::parse_task_packet: a malformed evidence row fails the whole parse", "[engine][execute][state]") {
  // Missing "provenance" on the one owning_plans row.
  constexpr std::string_view fixture =
      R"JSON({"input":{"task_id":1,"status":"todo","title":"T","body":"B","next_action":"N","acceptance_criteria":"A",
"owning_plans":[{"kind":"plan","id":1,"locator":"plan:1","text":"t","source_digest":"d",
"current_digest":"d","required":true,"covered":true,"status":"current"}],"anchor_plans":[],
"citations":[],"decisions":[],"questions":[],"scenarios":[],"dependencies":[],"touches":[],
"claims":[],"validation_gates":[],"facts":[]},"digest":"dg","reasons":[]})JSON";

  auto const got = ex::state::parse_task_packet(fixture);
  CHECK(!got.has_value());
}
