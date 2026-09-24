// @file schema.t.cpp
// @brief Pure JSON-decode fixtures for `planar::engine::execute::schema` —
// the slice of `schema.zig` ported at task 6125 for `ctx.brief`'s
// `planar-agent schema` read. No subprocess, no live binary; every fixture
// below mirrors a real shape captured by `schema.zig`'s own fixture tests
// (`zig/src/cmd/planar-execute/schema.zig`).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;

namespace {

namespace ex = planar::engine::execute;

/// @brief Trimmed real fixture, mirroring `schema.zig`'s
/// `fixture_agent_schema` (captured from `./bin/planar-agent schema`).
constexpr std::string_view k_agent_schema_fixture = R"JSON({"schemaVersion":1,"layout":"flat","root":"planar-agent","commands":[
  {"name":"planar-agent","command":"planar-agent","subcommands":["complete","heartbeat","pull"],"flags":[],"hidden":false},
  {"name":"complete","command":"planar-agent complete","subcommands":[],"hidden":false,"flags":[
    {"long":"--claim","aliases":[],"short":null,"required":true,"description":"Claim token returned by pull/claim"},
    {"long":"--summary","aliases":[],"short":null,"required":false,"description":"Free-text completion summary"},
    {"long":"--json","aliases":[],"short":null,"required":false,"description":""}
  ]},
  {"name":"heartbeat","command":"planar-agent heartbeat","subcommands":[],"hidden":false,"flags":[
    {"long":"--claim","required":true,"description":"Claim token to refresh"},
    {"long":"--ttl","required":false,"description":"New TTL"}
  ]}
]})JSON";

} // namespace

TEST_CASE("schema::parse_raw_schema decodes top-level fields and the command count", "[engine][execute][schema]") {
  auto const got = ex::schema::parse_raw_schema(k_agent_schema_fixture);
  REQUIRE(got.has_value());
  CHECK(got->schema_version == 1);
  CHECK(got->layout == "flat");
  CHECK(got->root == "planar-agent");
  CHECK(got->commands.size() == 3);
}

TEST_CASE("schema::bin_schema::find_command locates 'planar-agent complete' with its flags", "[engine][execute][schema]") {
  auto const raw = ex::schema::parse_raw_schema(k_agent_schema_fixture);
  REQUIRE(raw.has_value());
  ex::schema::bin_schema const bs{*raw};

  CHECK(bs.root() == "planar-agent");
  CHECK(bs.schema_version() == 1);
  CHECK(bs.commands().size() == 3);

  auto const* cmd = bs.find_command("planar-agent complete");
  REQUIRE(cmd != nullptr);
  CHECK(cmd->name == "complete");
  REQUIRE(cmd->flags.size() == 3);
  CHECK(cmd->flags[0].long_name == "--claim");
  CHECK(cmd->flags[0].required);
  CHECK(cmd->flags[1].long_name == "--summary");
  CHECK(!cmd->flags[1].required);

  CHECK(bs.find_command("planar-agent nonexistent") == nullptr);
}

TEST_CASE("schema: 'short' parses as a JSON STRING, not a number (etcli-zig emitter shape)", "[engine][execute][schema]") {
  // Regression fixture matching schema.zig's own task-3235 regression test:
  // the emitter serializes `short` as `"short":"v"`, never a bare byte.
  constexpr std::string_view fixture = R"JSON({"schemaVersion":1,"layout":"flat","root":"planar","commands":[
    {"name":"verbose","command":"planar verbose","subcommands":[],"hidden":false,"flags":[
      {"long":"--verbose","aliases":[],"short":"v","required":false,"description":"Enable verbose output"}
    ]}
  ]})JSON";
  auto const                 got     = ex::schema::parse_raw_schema(fixture);
  REQUIRE(got.has_value());
  REQUIRE(got->commands.size() == 1);
  REQUIRE(got->commands[0].flags.size() == 1);
  REQUIRE(got->commands[0].flags[0].short_name.has_value());
  CHECK(*got->commands[0].flags[0].short_name == "v");
}

TEST_CASE("schema::parse_raw_schema: a hidden command's hidden flag survives decode", "[engine][execute][schema]") {
  constexpr std::string_view fixture = R"JSON({"schemaVersion":1,"layout":"flat","root":"planar-agent","commands":[
    {"name":"internal-debug","command":"planar-agent internal-debug","subcommands":[],"hidden":true,"flags":[]}
  ]})JSON";
  auto const                 got     = ex::schema::parse_raw_schema(fixture);
  REQUIRE(got.has_value());
  REQUIRE(got->commands.size() == 1);
  CHECK(got->commands[0].hidden);
}

TEST_CASE("schema::parse_raw_schema: malformed JSON is rejected", "[engine][execute][schema]") {
  auto const got = ex::schema::parse_raw_schema("{ not json {{{{");
  CHECK(!got.has_value());
}

TEST_CASE("schema::parse_raw_schema: a command missing 'command' is rejected", "[engine][execute][schema]") {
  // `parse_command_entry` requires BOTH `name` and `command`; this fixture
  // is otherwise well-formed but omits `command` on the one entry.
  constexpr std::string_view fixture = R"JSON({"schemaVersion":1,"layout":"flat","root":"planar-agent","commands":[
    {"name":"complete","subcommands":[],"hidden":false,"flags":[]}
  ]})JSON";
  auto const                 got     = ex::schema::parse_raw_schema(fixture);
  CHECK(!got.has_value());
}
