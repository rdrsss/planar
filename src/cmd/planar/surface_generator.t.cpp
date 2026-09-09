// @file surface_generator.t.cpp
// @brief The `scripts/gen-cli-surface.py` generator and the shape of the
// `k_unported` inventories it writes into each binary's `surface.cpp`.
//
// ## Provenance
//
// This case used to live in `statediff.t.cpp`, the C++/Zig DATABASE-STATE
// differential lane. That lane and its oracle were deleted at the M10
// cutover (task 6045, decisions 963/982) — with one implementation there is
// no differential to run. This case survived the deletion because its
// subject is NOT cross-implementation agreement: it is the generator's own
// two output branches, exercised over synthetic catalogs it writes itself.
// It never read the oracle and does not need one.
//
// ## What it pins
//
// `unported_paths()` is what makes a declared-but-unported leaf exit 64 with
// "not implemented in this build" rather than crash or silently succeed. The
// generator has two branches for it, and the EMPTY one is the one that broke:
// a bare `return k_unported` over a zero-length array is ill-formed, so the
// generator emits a one-element array narrowed with `.first(0)` instead.
// Nothing else in the tree checks that the empty branch stays well-formed,
// and a regression there would not surface until some binary's last unported
// leaf landed.
//
// `planar:explore` is the only entry the tree still carries, deferred by
// decision 980 rather than pending — see `src/cmd/planar/surface.cpp`.

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

/// @brief Parse the generated `k_unported` initializer, ignoring comments.
///
/// Deliberately scans the whole initializer rather than one line at a time:
/// the generator is allowed to coalesce entries onto a single line, and a
/// line-oriented parser silently read only the first of them once.
/// @param source The generated `surface.cpp` to parse.
/// @return The declared paths in sorted order, or `std::nullopt` on a
/// malformed or duplicate-bearing initializer.
auto generated_unported(const std::filesystem::path& source) -> std::optional<std::vector<std::string>> {
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

TEST_CASE("cli surface generator emits safe empty inventories and preserves non-empty ones", "[cmd][generator]") {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_surface_generator_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code discard;
  std::filesystem::create_directories(root / "scripts", discard);
  for (auto const& binary : {"planar", "planar-agent", "planar-watch"}) {
    std::filesystem::create_directories(root / "src" / "cmd" / binary, discard);
    std::ofstream dispatch(root / "src" / "cmd" / binary / "dispatch.cpp");
    dispatch << "namespace x { void f() {} }\n";
  }
  REQUIRE(std::filesystem::copy_file(target_source_root() / "scripts/gen-cli-surface.py", root / "scripts/gen-cli-surface.py",
                                     std::filesystem::copy_options::overwrite_existing, discard));
  auto const catalog_dir = root / "catalog";
  std::filesystem::create_directories(catalog_dir, discard);
  auto write_catalog = [&](std::string_view binary, std::string_view command, std::string_view leaf) {
    std::ofstream out(catalog_dir / ("oracle-" + std::string{binary} + ".json"));
    out << "{\"commands\":[{\"command\":\"" << command
        << "\",\"summary\":\"root\",\"description\":\"root\",\"path\":[],\"flags\":[],\"positionals\":[],\"subcommands\":[]},{"
           "\"command\":\""
        << command << " " << leaf << "\",\"summary\":\"leaf\",\"description\":\"leaf\",\"path\":[\"" << leaf
        << "\"],\"flags\":[],\"positionals\":[],\"subcommands\":[]}] }";
  };
  write_catalog("planar", "planar", "ported");
  write_catalog("planar-agent", "planar-agent", "ported");
  write_catalog("planar-watch", "planar-watch", "pending");
  // `pending` is intentionally absent from dispatch, while `ported` is
  // registered below. This distinguishes the generator's empty and
  // non-empty branches without any live DB.
  for (auto const& binary : {"planar", "planar-agent"}) {
    std::ofstream dispatch(root / "src" / "cmd" / binary / "dispatch.cpp", std::ios::app);
    dispatch << "table.emplace(\"ported\", f);\n";
  }
  auto const command =
      std::format("cd '{}' && python3 scripts/gen-cli-surface.py '{}' >/dev/null", root.string(), catalog_dir.string());
  REQUIRE(std::system(command.c_str()) == 0);
  auto const empty   = source_text(root / "src/cmd/planar-agent/surface.cpp");
  auto const pending = source_text(root / "src/cmd/planar-watch/surface.cpp");
  // Both live under this test's own synthetic `root`, written by the
  // generator invocation above — unrelated to the real repository's
  // `src/cmd/planar-watch/surface.cpp`, which task 6613 (M11.1) deleted.
  // This case never reads the real tree, so that deletion does not touch
  // it; see the next TEST_CASE for the one that does.
  REQUIRE(empty.has_value());
  REQUIRE(pending.has_value());
  CHECK(empty->contains("k_unported[] = {std::string_view{}}"));
  CHECK(empty->contains(".first(0)"));
  CHECK(generated_unported(root / "src/cmd/planar-agent/surface.cpp") == std::vector<std::string>{});
  CHECK(pending->contains("\"pending\""));
  CHECK(generated_unported(root / "src/cmd/planar-watch/surface.cpp") == std::vector<std::string>{"pending"});
  // Break probe: the prior bare `return k_unported` branch cannot emit the
  // zero-length span string this test pins, so this case fails if restored.
  std::filesystem::remove_all(root, discard);
}

TEST_CASE("the checked-in unported inventories carry only decision-980's deferred leaf", "[cmd][generator]") {
  // The live half of the same subject. `statediff.t.cpp` used to assert
  // these counts as one of the oracle-retirement gate's three conditions
  // (decision 963/982, condition 2: "the unported inventory contains only
  // `explore`"). The gate is retired with its subject, but the FACT it
  // gated is a standing property of this tree and is pinned here directly.
  auto const planar_unported = generated_unported(target_source_root() / "src/cmd/planar/surface.cpp");
  // Neither `planar-agent` nor `planar-watch` ships a generated
  // `surface.cpp` any more — task 6613 (M11.1) folded `planar-watch`'s
  // `node_spec` table into `tree.cpp` and task 6614 (M11.2) did the same
  // for `planar-agent`, each moving the functions this parser actually
  // cares about (`surface_summaries` where it survives, `unported_paths`
  // always) into a self-contained `surface.cppm`, in the exact same
  // scanner-recognized empty-array shape `generated_unported` parses.
  // Repointed rather than dropped: the property being pinned (nothing
  // remains declared-but-unported on either binary) still holds and is
  // still worth catching a regression on.
  auto const agent_unported = generated_unported(target_source_root() / "src/cmd/planar-agent/surface.cppm");
  auto const watch_unported = generated_unported(target_source_root() / "src/cmd/planar-watch/surface.cppm");
  REQUIRE(planar_unported.has_value());
  REQUIRE(agent_unported.has_value());
  REQUIRE(watch_unported.has_value());

  CHECK(*planar_unported == std::vector<std::string>{"explore"});
  CHECK(agent_unported->empty());
  CHECK(watch_unported->empty());

  // The generated empty inventory must retain a scanner-recognized named
  // initializer while exposing no runtime elements. This protects the
  // generator's zero-list branch from regressing to an ill-formed array.
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
