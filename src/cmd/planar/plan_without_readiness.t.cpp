// @file plan_without_readiness.t.cpp
// @brief A plan has no readiness: the built binaries show no plan-readiness
// reason anywhere, and a plan with no artifacts has tasks an agent can claim.
//
// The plan-level packet gates (the spec pipeline's strict preview, review
// rubric, locked decisions, coverage, apply boundary and the rest) are gone.
// These cases drive the real `planar` and `planar-agent` binaries the way an
// operator does, against a scratch arena, and read what they print.

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;

/// @brief The reason names the plan-level packet used to emit.
constexpr std::array<std::string_view, 16> k_removed_reasons{
    "missing_strict_preview",   "missing_review_rubric",    "missing_reviewed_artifacts",
    "missing_locked_decisions", "missing_required_outputs", "missing_goal",
    "missing_scope_facts",      "missing_source_artifacts", "missing_artifact_digests",
    "missing_coverage",         "incomplete_coverage",      "missing_apply_boundary",
    "invalid_apply_boundary",   "invalid_locked_decisions", "invalid_strict_preview",
    "non_current_artifacts"};

/// @brief A scratch arena with the two binaries a claim needs.
struct world {
  parity::arena         arena = parity::make_arena("plan_without_readiness");
  std::filesystem::path root  = arena.cpp_root;
  int                   seq   = 0;

  /// @brief Run one verb and return its capture.
  /// @param bin The binary to run.
  /// @param args The arguments after the binary name.
  /// @return The exit code and both streams.
  auto run(std::filesystem::path const& bin, std::vector<std::string> const& args) -> parity::capture {
    auto got = parity::run_pinned(bin, args, root, std::format("pwr{}", ++seq));
    INFO(bin.filename().string() << " " << args.front() << " -> " << got.code << ": " << got.err << got.out);
    return got;
  }

  auto planar(std::vector<std::string> const& args) -> parity::capture {
    return run(std::filesystem::path{PLANAR_CPP_BIN}, args);
  }

  auto planar_agent(std::vector<std::string> const& args) -> parity::capture {
    return run(std::filesystem::path{PLANAR_AGENT_CPP_BIN}, args);
  }
};

/// @brief Names the first removed reason that appears in `text`, or the empty string.
/// @param text The output to scan.
/// @return A removed reason found in `text`, or empty.
auto find_removed_reason(std::string_view text) -> std::string {
  for (auto const reason : k_removed_reasons) {
    if (text.contains(reason)) {
      return std::string{reason};
    }
  }
  return {};
}

} // namespace

TEST_CASE("a fresh plan shows no plan-readiness reason and its task is claimable",
          "[cmd][plan][packet][claim][readiness][7478]") {
  world w;
  REQUIRE(w.planar({"init", "--skip-project", "--allow-no-repo"}).code == 0);
  REQUIRE(w.planar({"plan", "create", "Bare plan"}).code == 0);
  REQUIRE(w.planar({"task", "add", "Bare task", "--plan", "1"}).code == 0);

  // No artifact, decision, scenario or ingest has touched this plan. Every
  // read an operator or agent makes about it, in both renderings, names none
  // of the removed reasons.
  for (auto const& args : std::vector<std::vector<std::string>>{{"task", "packet", "1", "--json"},
                                                                {"task", "packet", "1"},
                                                                {"plan", "show", "1", "--json"},
                                                                {"plan", "show", "1"},
                                                                {"plan", "next", "1", "--json"},
                                                                {"plan", "next", "1"}}) {
    auto const got = w.planar(args);
    REQUIRE(got.code == 0);
    CHECK(find_removed_reason(got.out + got.err).empty());
  }

  auto const packet = w.planar({"task", "packet", "1", "--json"});
  CHECK(packet.out.contains(R"("ready":true)"));
  CHECK(packet.out.contains(R"("reasons":[])"));

  // The existing claim path takes the task.
  auto const claim = w.planar_agent(
      {"claim", "--entity", "task:1", "--vendor", "claude", "--role", "coder", "--ttl", "30m", "--no-locality-probe"});
  CHECK(claim.code == 0);
  CHECK(claim.out.contains("entity:task:1 status:active"));
  CHECK(find_removed_reason(claim.out + claim.err).empty());
}

TEST_CASE("there is no plan packet or plan readiness verb", "[cmd][plan][packet][readiness][removed][7478]") {
  world w;
  REQUIRE(w.planar({"init", "--skip-project", "--allow-no-repo"}).code == 0);
  REQUIRE(w.planar({"plan", "create", "Bare plan"}).code == 0);

  for (auto const* verb : {"packet", "readiness"}) {
    auto const got = w.planar({"plan", verb, "1"});
    CHECK(got.code == 2);
    CHECK(got.err.contains("not expected"));
    CHECK(got.out.empty());
  }

  // And neither verb is in the catalog an agent reads.
  auto const schema = w.planar({"schema"});
  REQUIRE(schema.code == 0);
  CHECK_FALSE(schema.out.contains("planar plan packet"));
  CHECK_FALSE(schema.out.contains("planar plan readiness"));
  CHECK(find_removed_reason(schema.out).empty());
}
