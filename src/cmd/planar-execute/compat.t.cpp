/// @file compat.t.cpp
/// @brief What makes two clients able to share one daemon, and what does not
///        (plan 1033 M2, task 6503).
///
/// Each case changes exactly ONE thing about the world and asserts that the
/// comparison notices it and names it. A tuple field that no case can move is
/// a field the comparison cannot really be said to cover.

import std;
import planar.cmd.planar_execute.compat;
import planar.cmd.planar_execute.host;
import planar.cmd.planar_execute.profile;

#include <catch2/catch_test_macros.hpp>

namespace {

using planar::cmd::execute::compare_tuples;
using planar::cmd::execute::compatibility_inputs;
using planar::cmd::execute::compatibility_tuple;
using planar::cmd::execute::compute_tuple;
using planar::cmd::execute::directory_digest;
using planar::cmd::execute::layout_for;
using planar::cmd::execute::mismatch_text;
using planar::cmd::execute::profile;
using planar::cmd::execute::record_tuple;
using planar::cmd::execute::recorded_tuple;

/// @brief A temp tree holding a profile's state, bundle and installed daemon.
class scratch_world {
  std::filesystem::path _root;

public:
  explicit scratch_world(std::string_view tag)
      : _root(std::filesystem::temp_directory_path() /
              std::format("planar-compat-{}-{}", tag, std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::filesystem::create_directories(_root / "state");
    std::filesystem::create_directories(_root / "bundles" / "supervision");
    std::ofstream(_root / "bundles" / "supervision" / "workflow.lua") << "return workflow.complete({})";
    std::ofstream(_root / "command-policy.json") << R"({"schema_version":1,"commands":[]})";
    // The installed daemon's layout: <prefix>/bin/centuriond beside
    // <prefix>/share/centurion/build-identity.json.
    std::filesystem::create_directories(_root / "prefix" / "bin");
    std::filesystem::create_directories(_root / "prefix" / "share" / "centurion");
    std::ofstream(_root / "prefix" / "bin" / "centuriond") << "#!/bin/sh\n";
    write_identity("v0.1.0-alpha.2", std::string(64, 'a'));
  }
  ~scratch_world() {
    std::error_code ignored;
    std::filesystem::remove_all(_root, ignored);
  }
  scratch_world(const scratch_world&)                    = delete;
  auto operator=(const scratch_world&) -> scratch_world& = delete;

  [[nodiscard]] auto root() const -> std::filesystem::path {
    return _root;
  }
  [[nodiscard]] auto daemon() const -> std::filesystem::path {
    return _root / "prefix" / "bin" / "centuriond";
  }

  auto write_identity(std::string_view tag, std::string_view digest) const -> void {
    std::ofstream(_root / "prefix" / "share" / "centurion" / "build-identity.json")
        << std::format(R"({{"tag":"{}","commit":"c","archive_sha256":"a","source":"source-build",)"
                       R"("binary_sha256":"{}","migrations_dir":"m"}})",
                       tag, digest);
  }

  [[nodiscard]] auto make_profile() const -> profile {
    profile value;
    value.name           = "default";
    value.configured     = true;
    value.state_dir      = (_root / "state").string();
    value.planar_db      = (_root / "planar.db").string();
    value.bundle         = (_root / "bundles").string();
    value.command_policy = (_root / "command-policy.json").string();
    return value;
  }

  [[nodiscard]] auto inputs() const -> compatibility_inputs {
    return compatibility_inputs{.daemon_           = daemon(),
                                .sibling_bin_dir_  = _root / "planar-bin",
                                .workbench_root_   = _root / "workbench",
                                .protocol_version_ = "centurion.v1"};
  }
};

} // namespace

TEST_CASE("a directory digest covers content, names and structure", "[execute][compat]") {
  const scratch_world world("digest");
  const auto          bundles = world.root() / "bundles";
  const auto          first   = directory_digest(bundles);
  REQUIRE_FALSE(first.empty());

  SECTION("editing a file changes it") {
    std::ofstream(bundles / "supervision" / "workflow.lua") << "return workflow.complete({changed = true})";
    CHECK(directory_digest(bundles) != first);
  }

  SECTION("renaming a file changes it, even with identical bytes") {
    std::filesystem::rename(bundles / "supervision" / "workflow.lua", bundles / "supervision" / "other.lua");
    CHECK(directory_digest(bundles) != first);
  }

  SECTION("adding a file changes it") {
    std::ofstream(bundles / "supervision" / "extra.lua") << "";
    CHECK(directory_digest(bundles) != first);
  }

  SECTION("an absent directory digests to empty, which is not any directory's digest") {
    CHECK(directory_digest(world.root() / "absent").empty());
  }

  SECTION("the same content digests the same twice") {
    CHECK(directory_digest(bundles) == first);
  }
}

TEST_CASE("a recorded tuple round-trips", "[execute][compat]") {
  const scratch_world world("roundtrip");
  const auto          layout = layout_for(world.make_profile());
  const auto          tuple  = compute_tuple(world.make_profile(), world.inputs());

  REQUIRE(record_tuple(layout, tuple).has_value());
  const auto read_back = recorded_tuple(layout);
  REQUIRE(read_back.has_value());
  CHECK(*read_back == tuple);
  CHECK(compare_tuples(read_back, tuple).empty());
}

TEST_CASE("no recorded tuple is compatible, not a refusal", "[execute][compat]") {
  // A daemon started before this check existed, or by a client that recorded
  // nothing, is not evidence of a conflict. Refusing on absence would make
  // every upgrade fail on its first run.
  const scratch_world world("absent");
  CHECK(compare_tuples(std::nullopt, compute_tuple(world.make_profile(), world.inputs())).empty());
}

TEST_CASE("every field that changes what a run means is compared", "[execute][compat]") {
  const scratch_world world("fields");
  const auto          layout   = layout_for(world.make_profile());
  const auto          recorded = compute_tuple(world.make_profile(), world.inputs());

  const auto differing_field = [&](const compatibility_tuple& current) -> std::string {
    const auto mismatches = compare_tuples(recorded, current);
    return mismatches.size() == 1 ? mismatches.front().field_ : std::format("<{} mismatches>", mismatches.size());
  };

  SECTION("a re-pinned or rebuilt daemon") {
    world.write_identity("v0.1.0-alpha.3", std::string(64, 'a'));
    CHECK(differing_field(compute_tuple(world.make_profile(), world.inputs())) == "daemon_build");
  }

  SECTION("the same tag rebuilt to different bytes") {
    world.write_identity("v0.1.0-alpha.2", std::string(64, 'b'));
    CHECK(differing_field(compute_tuple(world.make_profile(), world.inputs())) == "daemon_build");
  }

  SECTION("edited workflows") {
    std::ofstream(world.root() / "bundles" / "supervision" / "workflow.lua") << "return workflow.complete({v = 2})";
    CHECK(differing_field(compute_tuple(world.make_profile(), world.inputs())) == "bundle_digest");
  }

  SECTION("an edited command policy") {
    std::ofstream(world.root() / "command-policy.json") << R"({"schema_version":1,"commands":["git"]})";
    CHECK(differing_field(compute_tuple(world.make_profile(), world.inputs())) == "command_policy_digest");
  }

  SECTION("a different Planar database") {
    auto other      = world.make_profile();
    other.planar_db = (world.root() / "other.db").string();
    CHECK(differing_field(compute_tuple(other, world.inputs())) == "planar_db");
  }

  SECTION("a different workbench root") {
    auto inputs            = world.inputs();
    inputs.workbench_root_ = world.root() / "other-workbench";
    CHECK(differing_field(compute_tuple(world.make_profile(), inputs)) == "workbench_root");
  }

  SECTION("a different sibling binary directory") {
    auto inputs             = world.inputs();
    inputs.sibling_bin_dir_ = world.root() / "other-bin";
    CHECK(differing_field(compute_tuple(world.make_profile(), inputs)) == "sibling_bin_dir");
  }

  SECTION("a different wire contract") {
    auto inputs              = world.inputs();
    inputs.protocol_version_ = "centurion.v2";
    CHECK(differing_field(compute_tuple(world.make_profile(), inputs)) == "protocol_version");
  }
}

TEST_CASE("the refusal names every differing field and both values", "[execute][compat]") {
  const scratch_world world("text");
  const auto          recorded = compute_tuple(world.make_profile(), world.inputs());
  world.write_identity("v0.1.0-alpha.3", std::string(64, 'a'));
  auto other      = world.make_profile();
  other.planar_db = (world.root() / "other.db").string();

  const auto mismatches = compare_tuples(recorded, compute_tuple(other, world.inputs()));
  REQUIRE(mismatches.size() == 2);

  const auto text = mismatch_text(mismatches, "default");
  INFO(text);
  CHECK(text.contains("profile 'default'"));
  CHECK(text.contains("daemon_build"));
  CHECK(text.contains("planar_db"));
  // Both sides, so an operator can see WHICH database is which.
  CHECK(text.contains("other.db"));
  CHECK(text.contains("v0.1.0-alpha.3"));
  CHECK(text.contains("v0.1.0-alpha.2"));
}

TEST_CASE("a daemon with no build-identity file still compares by its path", "[execute][compat]") {
  // Falling back to an EMPTY identity would make two different installed
  // daemons look identical, which is the one thing this field exists to stop.
  const scratch_world world("no-identity");
  std::filesystem::remove(world.root() / "prefix" / "share" / "centurion" / "build-identity.json");

  const auto tuple = compute_tuple(world.make_profile(), world.inputs());
  CHECK_FALSE(tuple.daemon_build_.empty());
  CHECK(tuple.daemon_build_.contains("centuriond"));
}
