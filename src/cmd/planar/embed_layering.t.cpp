// @file embed_layering.t.cpp
// @brief No first-party translation unit may `#embed` a file from outside its
// own directory tree (plan 1080, task 7110).
//
// ## Why this is a test
//
// cmake/architecture.cmake enforces D15/D18 over the configured TARGET graph.
// An `#embed "../other/file"` is a dependency on another bucket's file that
// the graph cannot see: `engine_workspace` once embedded
// `../hostqueue/queue-rule.md`, coupling two engine buckets with no edge. A
// file two buckets need belongs in a layer-1 module that embeds it and
// exports the bytes (`planar.queuerule`), so both depend on it through a
// target the walk checks. A path that climbs out of its directory is the
// symptom, so this scan bans it outright.

#include <catch2/catch_test_macros.hpp>

import std;

namespace {

/// @brief The checkout under test (set by this target's CMakeLists).
/// @return The repository root whose sources are scanned.
auto source_root() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_TARGET_SOURCE_ROOT};
}

/// @brief Whether a path is first-party C++ this rule governs.
/// @param p The candidate path.
/// @return True for a `.cpp`/`.cppm`/`.hpp` file.
auto first_party_source(const std::filesystem::path& p) -> bool {
  auto const ext = p.extension().string();
  return ext == ".cpp" || ext == ".cppm" || ext == ".hpp";
}

/// @brief The `#embed` operands that climb out of their directory.
/// @param line One source line.
/// @return True when the line is an `#embed` directive whose path contains `..`.
auto embed_escapes(std::string_view line) -> bool {
  auto const first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos || !line.substr(first).starts_with("#embed")) {
    return false;
  }
  return line.find("..") != std::string_view::npos;
}

/// @brief Every first-party source with an escaping `#embed`.
/// @return Repo-relative `path:line` strings, sorted.
auto offenders() -> std::vector<std::string> {
  std::vector<std::string> found;
  auto const               root = source_root();
  for (auto const& entry : std::filesystem::recursive_directory_iterator(root / "src")) {
    if (!entry.is_regular_file() || !first_party_source(entry.path())) {
      continue;
    }
    std::ifstream input(entry.path());
    if (!input) {
      continue;
    }
    std::string line;
    for (std::size_t n = 1; std::getline(input, line); ++n) {
      if (embed_escapes(line)) {
        found.push_back(std::format("{}:{}", std::filesystem::relative(entry.path(), root).string(), n));
      }
    }
  }
  std::ranges::sort(found);
  return found;
}

} // namespace

TEST_CASE("the embed scanner flags a path that climbs out of its directory", "[cmd][planar][style][7110]") {
  CHECK(embed_escapes("#embed \"../hostqueue/queue-rule.md\""));
  CHECK(embed_escapes("  #embed \"../../x.md\""));
  CHECK_FALSE(embed_escapes("#embed \"queue-rule.md\""));
  CHECK_FALSE(embed_escapes("// the old #embed \"../x\" is gone"));
}

TEST_CASE("no first-party source embeds a file outside its own directory [7110]", "[cmd][planar][style][7110]") {
  auto const found = offenders();
  INFO("an `#embed` that climbs out of its directory hides a cross-bucket dependency from "
       "cmake/architecture.cmake; move the file into a layer-1 module (see src/lib/queuerule/). Offenders:\n"
       << [&] {
            std::string s;
            for (auto const& f : found) {
              s += "  " + f + "\n";
            }
            return s;
          }());
  CHECK(found.empty());
}
