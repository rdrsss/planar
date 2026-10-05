// @file schema_select.t.cpp
// @brief Black-box tests for `planar-ext schema --command` / `--compact` (task 7204).

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built binary (set by this target's CMakeLists).
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

} // namespace

// --- `schema --command` and `schema --compact` (task 7204) ---------------
//
// Expectations are derived from the task's contract, not from the emitter:
// a single catalog object for one path, a two-key row per command, exit 2
// with an empty stdout for an unknown path, and a bare verb-shaped value
// (`task`) that is a lookup rather than a reordered subcommand.

namespace {

/// @brief Count non-overlapping occurrences of `needle` in `text`.
auto count_of(std::string_view text, std::string_view needle) -> std::size_t {
  std::size_t n = 0;
  for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

} // namespace

TEST_CASE("planar-ext schema --command emits exactly one command's object", "[cmd][ext][schema][7204]") {
  auto const arena = make_arena("schema7204one");
  auto const one =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-ext sync pull"}, arena.cpp_root, "one");
  CHECK(one.code == 0);
  CHECK(one.err.empty());
  CHECK(one.out.starts_with(R"({"name":")"));
  CHECK(one.out.ends_with("}\n"));
  CHECK(one.out.contains(R"("command":"planar-ext sync pull")"));
  CHECK(count_of(one.out, R"("command":")") == 1);
  CHECK(one.out.size() < 4096);

  // The relative spelling resolves to the same bytes.
  auto const rel = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=sync pull"}, arena.cpp_root, "rel");
  CHECK(rel.code == 0);
  CHECK(rel.out == one.out);

  // The object is the one the full catalog carries, byte for byte.
  auto const full = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  REQUIRE(full.code == 0);
  CHECK(full.out.contains(one.out.substr(0, one.out.size() - 1)));
}

TEST_CASE("planar-ext schema --command with an unknown path exits 2, names it, prints nothing", "[cmd][ext][schema][7204]") {
  auto const arena = make_arena("schema7204bad");
  auto const bad   = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-ext sync pull nonesuch"},
                                arena.cpp_root, "bad");
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err.contains("planar-ext sync pull nonesuch"));
}

TEST_CASE("planar-ext schema --compact is one two-key row per command", "[cmd][ext][schema][7204]") {
  auto const arena   = make_arena("schema7204compact");
  auto const full    = run_pinned(cpp_bin(), std::vector<std::string>{"schema"}, arena.cpp_root, "full");
  auto const compact = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact"}, arena.cpp_root, "compact");
  REQUIRE(full.code == 0);
  REQUIRE(compact.code == 0);
  CHECK(compact.err.empty());
  auto const commands = count_of(full.out, R"("path":[)");
  CHECK(commands > 1);
  CHECK(count_of(compact.out, R"({"command":")") == commands);
  CHECK(count_of(compact.out, R"(,"summary":")") == commands);
  CHECK_FALSE(compact.out.contains(R"("flags")"));
  CHECK_FALSE(compact.out.contains(R"("name":)"));
  CHECK(compact.out.size() < 40 * 1024);
  CHECK(compact.out.size() < full.out.size());

  // Both flags: that one command's row, bare.
  auto const row =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--compact", "--command", "sync pull"}, arena.cpp_root, "row");
  CHECK(row.code == 0);
  CHECK(row.out.starts_with(R"({"command":"planar-ext sync pull","summary":")"));
  CHECK(count_of(row.out, R"("command":")") == 1);
  CHECK(compact.out.contains(row.out.substr(0, row.out.size() - 1)));
}

TEST_CASE("planar-ext schema --command sync is a lookup, not the sync subcommand", "[cmd][ext][schema][7204]") {
  auto const arena = make_arena("schema7204edge");
  auto const bare  = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "sync"}, arena.cpp_root, "bare");
  auto const full =
      run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command", "planar-ext sync"}, arena.cpp_root, "full");
  CHECK(bare.code == 0);
  CHECK(bare.err.empty());
  CHECK(bare.out.starts_with(R"({"name":"sync",)"));
  CHECK(bare.out.contains(R"("command":"planar-ext sync")"));
  CHECK(bare.out == full.out);
  // Same value placed before the verb, and in the `=` form.
  auto const before = run_pinned(cpp_bin(), std::vector<std::string>{"--command", "sync", "schema"}, arena.cpp_root, "before");
  CHECK(before.code == 0);
  CHECK(before.out == bare.out);
  auto const eq = run_pinned(cpp_bin(), std::vector<std::string>{"schema", "--command=sync"}, arena.cpp_root, "eq");
  CHECK(eq.out == bare.out);
}
