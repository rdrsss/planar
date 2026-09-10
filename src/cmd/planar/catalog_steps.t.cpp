// @file catalog_steps.t.cpp
// @brief The catalog-to-argv conversion and inventory gate in
// `catalog_steps.hpp`: what it admits, what it refuses, and that its
// refusals are exact rather than incidental.
//
// ## Provenance (plan 996, task 6045; decisions 963/982)
//
// These six cases used to live in `statediff.t.cpp`, the C++/Zig
// DATABASE-STATE differential lane. That lane and its oracle were deleted
// at the M10 cutover. The cases SURVIVED it because their subject is not
// cross-implementation agreement: every one of them builds its own catalog
// document with `catalog_fixture` or a literal, and none ever consulted
// `oracle_available()`, ran a second binary, or read `zig/`. What they
// grade is the conversion's own arms — invented argv, omitted leaves,
// forged argv under a retained path, catalog disagreement, argv boundaries,
// and the exactness of the exclusion partition.
//
// `catalog_steps.hpp` outlived the same deletion for the same reason; see
// its header for why its second-catalog parameter is now a fixture rather
// than an oracle.

#include <catch2/catch_test_macros.hpp>

import std;

#include "catalog_steps.hpp"
#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;
namespace state_catalog = planar::cmd::state_catalog;

/// @brief One verb invocation in a hand-authored ordered sequence.
struct step {
  std::vector<std::string> args; ///< The argv tail.
};

/// @brief Project manually ordered executions onto the eligible catalog leaves they cover.
///
/// A manual sequence may add data-dependent arguments or optional flags, so
/// its raw argv is not the canonical generated argv. Resolution therefore
/// starts from the executed prefix and returns the canonical catalog step.
/// The resulting inventory is a set: repeated `list` probes stay in the
/// ordered sequence, but cover one catalog leaf exactly once.
/// @param manual The ordered, hand-authored invocations.
/// @param eligible The catalog-derived eligible leaves.
/// @param error Receives a fail-closed diagnostic.
/// @return The covered leaves, or unset on a resolution failure.
auto stateful_catalog_inventory(std::span<const step> manual, std::span<const state_catalog::step> eligible, std::string& error)
    -> std::optional<std::vector<state_catalog::step>> {
  std::map<std::string, state_catalog::step, std::less<>> resolved;
  for (auto const& invocation : manual) {
    auto leaf = state_catalog::stateful_leaf(invocation.args, eligible, error);
    if (!leaf) {
      if (!error.empty()) {
        return std::nullopt;
      }
      continue;
    }
    auto key = state_catalog::detail::path_key(leaf->path, error);
    if (!key) {
      return std::nullopt;
    }
    resolved.emplace(*key, std::move(*leaf));
  }

  std::vector<state_catalog::step> out;
  out.reserve(resolved.size());
  for (auto const& [_, leaf] : resolved) {
    out.push_back(leaf);
  }
  return out;
}

/// @brief Form the catalog partition while retaining the ordered manual sequence.
/// @param eligible Every eligible catalog leaf.
/// @param stateful The leaves the manual sequence already covers.
/// @param error Receives a fail-closed diagnostic.
/// @return The leaves left for generation, or unset when `stateful` repeats one.
auto generated_catalog_inventory(std::span<const state_catalog::step> eligible, std::span<const state_catalog::step> stateful,
                                 std::string& error) -> std::optional<std::vector<state_catalog::step>> {
  std::set<std::string, std::less<>> stateful_keys;
  for (auto const& item : stateful) {
    auto key = state_catalog::detail::path_key(item.path, error);
    if (!key || !stateful_keys.insert(*key).second) {
      if (error.empty()) {
        error = "stateful catalog inventory repeats a leaf";
      }
      return std::nullopt;
    }
  }

  std::vector<state_catalog::step> out;
  for (auto const& item : eligible) {
    auto key = state_catalog::detail::path_key(item.path, error);
    if (!key) {
      return std::nullopt;
    }
    if (!stateful_keys.contains(*key)) {
      out.push_back(item);
    }
  }
  return out;
}

/// @brief Build a minimal schema document for catalog-conversion guards.
/// @param paths One path-token array per leaf.
/// @return A valid catalog containing optional-only leaf entries.
auto catalog_fixture(std::span<const std::vector<std::string>> paths) -> std::string {
  std::string document = R"({"commands":[)";
  for (std::size_t i = 0; i < paths.size(); ++i) {
    auto encoded = glz::write_json(paths[i]);
    REQUIRE(encoded.has_value());
    if (i != 0) {
      document += ',';
    }
    document += std::format(R"({{"path":{},"subcommands":[],"positionals":[],"flags":[]}})", *encoded);
  }
  return document + "]}";
}

} // namespace

TEST_CASE("state catalog: invented and omitted leaves fail the inventory gate", "[cmd][state][catalog]") {
  std::vector<std::vector<std::string>> const paths{{"one"}, {"two"}};
  auto const                                  catalog = catalog_fixture(paths);
  std::string                                 error;
  auto                                        generated = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(generated.has_value());
  REQUIRE(generated->size() == 2);

  auto invented = *generated;
  invented.push_back({.path = {"invented"}, .args = {"invented"}});
  CHECK_FALSE(state_catalog::verify_inventory(invented, catalog, catalog, error));
  CHECK(error.contains("invented"));

  auto omitted = *generated;
  omitted.pop_back();
  CHECK_FALSE(state_catalog::verify_inventory(omitted, catalog, catalog, error));
  CHECK(error.contains("missing"));
}

TEST_CASE("state catalog: retained catalog path cannot authorize invented argv", "[cmd][state][catalog]") {
  std::vector<std::vector<std::string>> const paths{{"one"}, {"two"}};
  auto const                                  catalog = catalog_fixture(paths);
  std::string                                 error;
  auto                                        generated = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(generated.has_value());

  auto forged         = *generated;
  forged.front().args = {"invented"}; // Path metadata remains untouched; argv is what executes.
  CHECK_FALSE(state_catalog::verify_inventory(forged, catalog, catalog, error));
  CHECK(error.contains("generated argv is not a catalog leaf"));
}

TEST_CASE("state catalog: an eligibility disagreement fails before steps run", "[cmd][state][catalog]") {
  // RENAMED AT THE M10 CUTOVER (task 6045). This case was called "C++ and
  // Zig eligibility disagreement fails before steps run", and the name was
  // the ONLY thing in it that ever mentioned the oracle: both sides are
  // `catalog_fixture` documents built two lines above, and the conversion
  // under test never learns what produced them. What it pins is that a
  // disagreement between the two catalogs is caught in CONVERSION rather
  // than discovered later by a step that ran against a leaf only one side
  // declares. That property is a property of `generated_steps`, and it did
  // not leave with the second implementation.
  std::vector<std::vector<std::string>> const declared{{"one"}, {"two"}};
  std::vector<std::vector<std::string>> const narrower{{"one"}};
  std::string                                 error;
  CHECK_FALSE(state_catalog::generated_steps(catalog_fixture(declared), catalog_fixture(narrower), error).has_value());
  CHECK(error.contains("eligible catalog inventory differs"));
}

TEST_CASE("state catalog: argv boundaries remain discrete and malformed input fails closed", "[cmd][state][catalog]") {
  auto const                     arena     = make_arena("catalogsteps_argv");
  auto const                     evaluated = arena.cpp_root / "was-evaluated";
  std::vector<std::string> const raw{
      "leaf", "has space", "single'quote", "double\"quote", "", ",;|:", std::format("$(touch {})", evaluated.string())};
  std::vector<std::vector<std::string>> const paths{raw};
  auto const                                  catalog = catalog_fixture(paths);

  std::string error;
  auto        generated = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(generated.has_value());
  REQUIRE(generated->size() == 1);
  CHECK(generated->front().args == raw);

  // `run_pinned` is the only shell boundary here. Its input is the generated
  // vector itself, and the witness proves that every element stays one argv
  // value while the shell metacharacters stay data, not code.
  auto const witness = arena.cpp_root / "proj" / "argv-witness";
  auto const script  = arena.cpp_root / "proj" / "capture-argv.sh";
  {
    std::ofstream out(script);
    REQUIRE(out.good());
    out << "#!/bin/sh\nprintf '%s\\n' \"$@\" > " << planar::cmd::parity::shell_quote(witness.string()) << "\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
  auto const captured = run_pinned(script, generated->front().args, arena.cpp_root, "argv_boundaries");
  REQUIRE(captured.code == 0);
  std::string expected;
  for (auto const& value : raw) {
    expected += value + '\n';
  }
  CHECK(planar::cmd::parity::read_all(witness) == expected);
  CHECK_FALSE(std::filesystem::exists(evaluated));

  // Conversion accepts catalog bytes, not a shell command. A parse or shape
  // failure returns before any step can launch a binary.
  CHECK_FALSE(state_catalog::generated_steps(R"({"commands":[)", catalog, error).has_value());
  CHECK(error.contains("malformed"));
  CHECK_FALSE(state_catalog::detail::leaves(R"({"commands":[)", error).has_value());
  CHECK_FALSE(state_catalog::generated_steps(R"({"commands":[{"path":[1],"subcommands":[],"positionals":[],"flags":[]}]})",
                                             catalog, error)
                  .has_value());
  CHECK(error.contains("non-string"));
}

TEST_CASE("state catalog: exceptions form a disjoint exact coverage partition", "[cmd][state][catalog]") {
  auto const  catalog = R"({"commands":[
    {"path":["plan","show"],"subcommands":[],"positionals":[{"required":true}],"flags":[]},
    {"path":["config","edit"],"subcommands":[],"positionals":[],"flags":[]},
    {"path":["explore"],"subcommands":[],"positionals":[],"flags":[]},
    {"path":["list"],"subcommands":[],"positionals":[],"flags":[{"required":false,"long":"--json"}]},
    {"path":["workspace","init"],"subcommands":[],"positionals":[],"flags":[{"required":false,"long":"--json"}]}
  ]})";
  std::string error;
  auto        eligible = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(eligible.has_value());
  REQUIRE(eligible->size() == 2);

  std::vector<step> const manual{{{"list", "--status", "", "--json"}}};
  auto                    stateful = stateful_catalog_inventory(manual, *eligible, error);
  REQUIRE(stateful.has_value());
  REQUIRE(stateful->size() == 1);
  CHECK(stateful->front().path == std::vector<std::string>{"list"});

  auto generated = generated_catalog_inventory(*eligible, *stateful, error);
  REQUIRE(generated.has_value());
  REQUIRE(generated->size() == 1);
  CHECK(generated->front().path == std::vector<std::string>{"workspace", "init"});

  std::vector<state_catalog::step> const malformed{{.args = {"plan", "show"}}};
  CHECK(state_catalog::verify_partition(*generated, *stateful, malformed, catalog, catalog, error));

  auto overlapping = *generated;
  overlapping.push_back(stateful->front());
  CHECK_FALSE(state_catalog::verify_partition(overlapping, *stateful, malformed, catalog, catalog, error));
  CHECK(error.contains("overlap"));

  auto bad_malformed         = malformed;
  bad_malformed.front().args = {"list"};
  CHECK_FALSE(state_catalog::verify_partition(*generated, *stateful, bad_malformed, catalog, catalog, error));
  CHECK(error.contains("malformed argv overlaps"));
}

TEST_CASE("state catalog: exclusions are explicit and workspace init remains eligible", "[cmd][state][catalog]") {
  auto const  catalog = R"({"commands":[
    {"path":["explore"],"subcommands":[],"positionals":[],"flags":[]},
    {"path":["config","edit"],"subcommands":[],"positionals":[],"flags":[]},
    {"path":["schema"],"subcommands":[],"positionals":[],"flags":[]},
    {"path":["workspace","init"],"subcommands":[],"positionals":[],"flags":[{"required":false,"long":"--json"}]}
  ]})";
  std::string error;
  auto        eligible = state_catalog::generated_steps(catalog, catalog, error);
  REQUIRE(eligible.has_value());
  REQUIRE(eligible->size() == 1);
  CHECK(eligible->front().args == std::vector<std::string>{"workspace", "init", "--json"});

  auto excluded = state_catalog::excluded_steps(catalog, catalog, error);
  REQUIRE(excluded.has_value());
  REQUIRE(excluded->size() == 3);
  CHECK(excluded->at(0).path == std::vector<std::string>{"config", "edit"});
  CHECK(excluded->at(0).reason == "interactive-editor");
  CHECK(excluded->at(1).path == std::vector<std::string>{"explore"});
  CHECK(excluded->at(1).reason == "deferred-by-decision980");
  CHECK(excluded->at(2).path == std::vector<std::string>{"schema"});
  CHECK(excluded->at(2).reason == "output-only");
}
