// @file format_to_ban.t.cpp
// @brief No first-party translation unit may append through
// `std::format_to(std::back_inserter(...), ...)`.
//
// ## Why this is a test and not only a style rule
//
// The pinned libc++ (23.1.1) backs a `back_insert_iterator` with a fixed
// 256-byte stack buffer (`__container_inserter_buffer::__small_buffer_`).
// Its bulk copy can leave that buffer EXACTLY full without flushing; the
// next literal character of the format string is then written through
// `push_back`, which writes before it checks, lands one byte past the
// array, and kills the process in `__vformat_to` via `__stack_chk_fail`
// (llvm/llvm-project#154670).
//
// The trigger is an argument whose BYTE length is a non-zero multiple of
// 256 with format-string text after it. Measured on this toolchain:
// 253/254/255/257/511/513 are fine, 256/512/1024 abort. Task 6880 found
// it the expensive way — `planar workbench status/push/pull` aborted for
// one plan because a scenario body was 255 characters but 256 bytes (a
// two-byte `§`), and the failure looked like data corruption rather than
// a formatting call.
//
// `std::format`, `std::vformat` and `std::print` allocate instead and are
// immune, so `out += std::format(...)` is the safe append. A prose rule in
// CLAUDE.md cannot catch a reintroduction; this can. Delete this test only
// when the pinned toolchain no longer carries the defect.

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
/// @return True for a `.cpp`/`.cppm`/`.hpp` file under `src/`.
auto first_party_source(const std::filesystem::path& p) -> bool {
  auto const ext = p.extension().string();
  return ext == ".cpp" || ext == ".cppm" || ext == ".hpp";
}

/// @brief Every first-party source naming the banned append form.
/// @return Repo-relative paths, each with the 1-based line number.
auto offenders() -> std::vector<std::string> {
  // Matched as two fragments so this file's own prose cannot trip it.
  constexpr std::string_view k_call   = "format_to";
  constexpr std::string_view k_target = "back_inserter";

  std::vector<std::string> found;
  auto const               root = source_root();
  for (auto const& entry : std::filesystem::recursive_directory_iterator(root / "src")) {
    if (!entry.is_regular_file() || !first_party_source(entry.path())) {
      continue;
    }
    if (entry.path().filename() == "format_to_ban.t.cpp") {
      continue;
    }
    std::ifstream input(entry.path());
    if (!input) {
      continue;
    }
    std::string line;
    for (std::size_t n = 1; std::getline(input, line); ++n) {
      auto const call = line.find(k_call);
      if (call == std::string::npos || line.find(k_target, call) == std::string::npos) {
        continue;
      }
      // Prose about the hazard is not the hazard: every file that explains
      // why this rule exists names the banned form in a comment. Skip a
      // `//` comment opened before the match, and block-comment
      // continuation lines.
      auto const slashes = line.find("//");
      if (slashes != std::string::npos && slashes < call) {
        continue;
      }
      auto const first = line.find_first_not_of(" \t");
      if (first != std::string::npos && line[first] == '*') {
        continue;
      }
      found.push_back(std::format("{}:{}", std::filesystem::relative(entry.path(), root).string(), n));
    }
  }
  std::ranges::sort(found);
  return found;
}

} // namespace

TEST_CASE("no first-party source appends through format_to(back_inserter(...)) [6880]", "[cmd][planar][style][6880]") {
  auto const found = offenders();
  INFO("libc++ 23.1.1 overflows its 256-byte stack buffer here "
       "(llvm/llvm-project#154670); append with `out += std::format(...)` instead. Offenders:\n"
       << [&] {
            std::string s;
            for (auto const& f : found) {
              s += "  " + f + "\n";
            }
            return s;
          }());
  CHECK(found.empty());
}
