// @file catalog_descriptions.t.cpp
// @brief Every flag and positional in every binary's `schema` catalog
// carries a help description (plan 1104, milestone 1105, task 7200).
//
// ## What this pins
//
// The flag-declaration helpers (`add_bool_flag`, the `planar::cmd::add_*`
// family, `planar-agent`'s shared helpers) take a REQUIRED description, so
// a new flag cannot be declared without one by accident. Two gaps stay open
// and this file is what covers them:
//
//   - A call site may still pass the explicit `k_undocumented` marker, and
//     raw `CLI::App::add_option(...)` calls in `planar-agent` and
//     `planar-ext` bypass the helpers entirely. Neither is visible to the
//     compiler. Both surface in the catalog as an empty description.
//   - The catalog is the one artifact all five binaries share, so it is the
//     place a single walk can hold them to one rule.
//
// ## The allowance
//
// `planar-agent`, `planar-ext` and `planar-watch` still have
// undocumented flags; their sweeps are later tasks in this milestone.
// `planar` and `planar-execute` have none and are held to the rule today. The
// allowance only shrinks: a binary in it that has NO empty description left
// fails the walk and tells the maintainer to delete its entry, so a finished
// sweep cannot silently regress behind a stale allowance.
//
// ## Provenance
//
// The fixture catalog below is shaped like `planar::cliapp::schema` output
// (`command`, `flags[*].long`, `positionals[*].name`, `description`); that
// shape was read from the five built binaries' `schema` output, not typed
// from the emitter.

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"
#include <glaze/glaze.hpp>

namespace {

using ::planar::cmd::parity::make_arena;
using ::planar::cmd::parity::run_pinned;

// One flag or positional whose description is empty.
struct empty_description {
  std::string command; // The command's full path, e.g. "planar-agent context resolve".
  std::string kind;    // "flag" or "positional".
  std::string name;    // "--json" or "finding".
};

// The result of walking one catalog.
struct walk_result {
  std::size_t                    walked = 0; // Flags plus positionals visited.
  std::vector<empty_description> empties;    // Those with an empty or missing description.
};

// Walk every command's flags and positionals in a `schema` document.
// Returns nullopt when the text is not a catalog.
auto walk_catalog(std::string const& text) -> std::optional<walk_result> {
  auto parsed = glz::read_json<glz::generic>(text);
  if (!parsed || !parsed->contains("commands")) {
    return std::nullopt;
  }
  walk_result out;
  for (auto const& entry : parsed->at("commands").get<glz::generic::array_t>()) {
    if (!entry.contains("command")) {
      continue;
    }
    auto const command = entry.at("command").get<std::string>();
    auto const visit   = [&](char const* list, char const* name_key, char const* kind) {
      if (!entry.contains(list)) {
        return;
      }
      for (auto const& item : entry.at(list).get<glz::generic::array_t>()) {
        ++out.walked;
        auto const description = item.contains("description") ? item.at("description").get<std::string>() : std::string{};
        if (description.empty()) {
          auto const name = item.contains(name_key) ? item.at(name_key).get<std::string>() : std::string{"<unnamed>"};
          out.empties.push_back({command, kind, name});
        }
      }
    };
    visit("flags", "long", "flag");
    visit("positionals", "name", "positional");
  }
  return out;
}

// The line a maintainer reads when a description is empty.
auto describe(empty_description const& e) -> std::string {
  return std::format("{}: {} {} has an empty description", e.command, e.kind, e.name);
}

// Binaries whose sweep has not landed. Remove an entry when its last empty
// description is fixed; the second case below fails until you do.
constexpr std::array<std::string_view, 3> k_allowed_empty = {"planar-agent", "planar-ext", "planar-watch"};

auto is_allowed(std::string_view binary) -> bool {
  return std::ranges::find(k_allowed_empty, binary) != k_allowed_empty.end();
}

struct binary_under_test {
  std::string           name;
  std::filesystem::path path;
};

auto binaries() -> std::vector<binary_under_test> {
  return {{"planar", PLANAR_CPP_BIN},
          {"planar-agent", PLANAR_AGENT_CPP_BIN},
          {"planar-watch", PLANAR_WATCH_CPP_BIN},
          {"planar-execute", PLANAR_EXECUTE_CPP_BIN},
          {"planar-ext", PLANAR_EXT_CPP_BIN}};
}

// Run `<bin> schema` in a fresh arena and walk its output.
auto walk_binary(binary_under_test const& bin) -> walk_result {
  auto const arena = make_arena(std::format("catdesc_{}", bin.name));
  auto const ran   = run_pinned(bin.path, std::vector<std::string>{"schema"}, arena.cpp_root, "schema");
  INFO(bin.name << " schema stderr: " << ran.err);
  REQUIRE(ran.code == 0);
  auto const walked = walk_catalog(ran.out);
  REQUIRE(walked.has_value());
  return *walked;
}

} // namespace

TEST_CASE("a catalog with one empty description is caught by name", "[catalog][descriptions]") {
  // One command with a documented flag, one subcommand with a documented
  // positional and exactly one flag whose description is empty.
  std::string const fixture = R"({"schemaVersion":1,"commands":[
    {"command":"tool","flags":[{"long":"--json","description":"Emit JSON."}],"positionals":[]},
    {"command":"tool sub","flags":[{"long":"--quiet","description":""}],
     "positionals":[{"name":"id","description":"The id."}]}]})";

  auto const walked = walk_catalog(fixture);
  REQUIRE(walked.has_value());
  CHECK(walked->walked == 3);
  REQUIRE(walked->empties.size() == 1);
  auto const message = describe(walked->empties.front());
  CAPTURE(message);
  CHECK(message.contains("tool sub"));
  CHECK(message.contains("--quiet"));
  CHECK(message == "tool sub: flag --quiet has an empty description");

  CHECK_FALSE(walk_catalog("not json").has_value());
  CHECK_FALSE(walk_catalog(R"({"nope":1})").has_value());
}

TEST_CASE("the catalog walk reaches every binary and fails on an empty description", "[catalog][descriptions]") {
  std::size_t total = 0;
  for (auto const& bin : binaries()) {
    auto const walked = walk_binary(bin);
    // Printed unconditionally: the scenario requires the remaining count per
    // binary to be reported, and Catch2 shows INFO/CAPTURE only on failure.
    std::println("catalog-descriptions: {} walked {} descriptions, {} empty", bin.name, walked.walked, walked.empties.size());
    CAPTURE(bin.name, walked.walked, walked.empties.size());
    total += walked.walked;
    if (is_allowed(bin.name)) {
      continue;
    }
    for (auto const& e : walked.empties) {
      FAIL_CHECK(describe(e));
    }
  }
  // The five live catalogs hold about 1,160 flag and positional entries
  // (measured 2026-10-04). A floor close to that catches a walk that
  // silently skipped a binary or a field; 630 would have let almost half the
  // surface vanish unnoticed.
  CAPTURE(total);
  REQUIRE(total >= 1100);
}

TEST_CASE("the catalog walk allows only the binaries still pending", "[catalog][descriptions]") {
  auto const all = binaries();
  for (auto const& name : k_allowed_empty) {
    auto const found = std::ranges::find_if(all, [&](binary_under_test const& bin) { return bin.name == name; });
    REQUIRE(found != all.end());
    auto const walked = walk_binary(*found);
    INFO(std::format("{} is in the allowance but has no empty description left; remove it from k_allowed_empty in "
                     "catalog_descriptions.t.cpp so the sweep cannot regress",
                     name));
    CHECK_FALSE(walked.empties.empty());
  }
  // planar-execute's sweep landed with its helpers, so it must never be allowed.
  CHECK_FALSE(is_allowed("planar-execute"));
}
