// @file surface_generator.t.cpp
// @brief The shape of the `k_unported` inventories each binary's
// `surface.cppm` carries.
//
// ## Provenance
//
// This file used to also carry a case exercising `scripts/gen-cli-surface.py`
// itself — copying the script into a synthetic temp-dir fixture, running it
// against hand-built catalogs, and pinning its two `k_unported` output
// branches (empty and non-empty). Plan 1051 M11 folded away every generated
// `node_spec` table the script fed (`planar-watch` at task 6613, `planar-agent`
// at 6614, `planar` across tasks 6631-6636), so the script had no more output
// to generate, and task 6616 deleted it along with `cliapp::apply_surface`,
// the mechanism it fed. That case tested the script and nothing else, so it
// was deleted with it rather than repointed.
//
// The case that remains pins a property of the checked-in tree, not of the
// generator: see its own comment for what it inherited from the retired
// `statediff.t.cpp` oracle-retirement gate.
//
// ## What it pins
//
// `unported_paths()` is what makes a declared-but-unported leaf exit 64 with
// "not implemented in this build" rather than crash or silently succeed.
// `planar:explore` is the only entry the tree still carries, deferred by
// decision 980 rather than pending — see `src/cmd/planar/surface.cppm`.

#include <catch2/catch_test_macros.hpp>

import std;

namespace {

/// @brief The worktree this test reads sources from (set by this target's CMakeLists).
/// @return The repository root of the checkout under test.
auto target_source_root() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_TARGET_SOURCE_ROOT};
}

/// @brief Read a target-worktree source file or return an empty optional.
/// @param path The file to read.
/// @return Its bytes, or `std::nullopt` when it cannot be opened.
auto source_text(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input}, {}};
}

/// @brief Parse the hand-authored `k_unported` initializer, ignoring comments.
///
/// Deliberately scans the whole initializer rather than one line at a time:
/// entries may be coalesced onto a single line, and a line-oriented parser
/// silently read only the first of them once.
/// @param source The hand-authored `surface.cppm` to parse.
/// @return The declared paths in sorted order, or `std::nullopt` on a
/// malformed or duplicate-bearing initializer.
auto parsed_unported(const std::filesystem::path& source) -> std::optional<std::vector<std::string>> {
  auto text = source_text(source);
  if (!text) {
    return std::nullopt;
  }
  auto const function = text->find("auto unported_paths()");
  auto const begin    = text->find("k_unported[] = {", function);
  auto const end      = text->find("};", begin);
  if (function == std::string::npos || begin == std::string::npos || end == std::string::npos) {
    return std::nullopt;
  }
  std::vector<std::string> out;
  std::istringstream       lines{text->substr(begin, end - begin)};
  for (std::string line; std::getline(lines, line);) {
    auto const comment = line.find("//");
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    for (std::size_t quote = line.find('"'); quote != std::string::npos;) {
      auto const close = line.find('"', quote + 1);
      if (close == std::string::npos) {
        return std::nullopt;
      }
      out.push_back(line.substr(quote + 1, close - quote - 1));
      quote = line.find('"', close + 1);
    }
  }
  std::ranges::sort(out);
  if (std::ranges::adjacent_find(out) != out.end()) {
    return std::nullopt;
  }
  return out;
}

} // namespace

TEST_CASE("the checked-in unported inventories carry only decision-980's deferred leaf", "[cmd][surface]") {
  // The live half of the same subject. `statediff.t.cpp` used to assert
  // these counts as one of the oracle-retirement gate's three conditions
  // (decision 963/982, condition 2: "the unported inventory contains only
  // `explore`"). The gate is retired with its subject, but the FACT it
  // gated is a standing property of this tree and is pinned here directly.
  // NO binary ships a generated `surface.cpp` any more — task 6613 (M11.1)
  // folded `planar-watch`'s `node_spec` table into `tree.cpp`, task 6614
  // (M11.2) did the same for `planar-agent`, and task 6636 (M11.3f)
  // finished `planar` by folding its last thirty-three entries out to the
  // handlers. Each moved the functions this parser actually cares about
  // (`surface_summaries` where it survives, `unported_paths` always) into
  // a self-contained `surface.cppm`, in the exact same
  // scanner-recognized array shape `parsed_unported` parses.
  // Repointed rather than dropped: the property being pinned (nothing
  // beyond decision 980's deferred leaf remains declared-but-unported on
  // any binary) still holds and is still worth catching a regression on.
  auto const planar_unported = parsed_unported(target_source_root() / "src/cmd/planar/surface.cppm");
  auto const agent_unported  = parsed_unported(target_source_root() / "src/cmd/planar-agent/surface.cppm");
  auto const watch_unported  = parsed_unported(target_source_root() / "src/cmd/planar-watch/surface.cppm");
  REQUIRE(planar_unported.has_value());
  REQUIRE(agent_unported.has_value());
  REQUIRE(watch_unported.has_value());

  CHECK(*planar_unported == std::vector<std::string>{"explore"});
  CHECK(agent_unported->empty());
  CHECK(watch_unported->empty());

  // The empty inventory must retain a scanner-recognized named initializer
  // while exposing no runtime elements. This protects the zero-list branch
  // from regressing to an ill-formed array.
  auto const agent_surface = source_text(target_source_root() / "src/cmd/planar-agent/surface.cppm");
  REQUIRE(agent_surface.has_value());
  CHECK(agent_surface->contains("k_unported[] = {std::string_view{}}"));
  CHECK(agent_surface->contains("std::span<std::string_view const>{k_unported}.first(0)"));

  for (auto const& landed : {"ingest", "run start", "run end", "dispatch preview", "dispatch confirm", "context add",
                             "context capsule", "context list", "context resolve"}) {
    INFO("landed agent path remained in unported inventory: " << landed);
    CHECK(std::ranges::find(*agent_unported, landed) == agent_unported->end());
  }
}
