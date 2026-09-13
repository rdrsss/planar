// Falsifies the two filesystem effects the import engine owns: a plain
// preview must not create operator state, while --interpret must atomically
// stage a request below the supplied (never real) PLANAR_HOME.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.importer;

namespace im = planar::engine::importer;

TEST_CASE("import staging separates no-interpret preview from pending handoff", "[engine][importer]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo" / "docs");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }
  {
    std::ofstream out(root / "repo" / "docs" / "guide.md");
    out << "body\n";
  }

  auto preview = im::run(root / "repo", home, false);
  REQUIRE(preview.has_value());
  CHECK(preview->mode_ == im::outcome::mode::skipped);
  CHECK(preview->request_.anchor_title == "Imported title");
  CHECK(preview->request_.docs_count == 2);
  // The cache key is a full lower-hex SHA-256 over canonical content, not a
  // short path-only fingerprint: changing a document invalidates the cache.
  CHECK(preview->request_.fingerprint.size() == 64);
  {
    std::ofstream out(root / "repo" / "docs" / "guide.md", std::ios::app);
    out << "changed\n";
  }
  auto changed = im::run(root / "repo", home, false);
  REQUIRE(changed.has_value());
  CHECK(changed->request_.fingerprint != preview->request_.fingerprint);
  CHECK_FALSE(std::filesystem::exists(home));

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  CHECK(staged->mode_ == im::outcome::mode::pending);
  CHECK(std::filesystem::is_regular_file(staged->pending_path));
  auto again = im::run(root / "repo", home, true);
  REQUIRE(again.has_value());
  CHECK(again->pending_path == staged->pending_path);
  std::filesystem::remove_all(root);
}

TEST_CASE("import rejects a malformed or mismatched interpretation cache before apply", "[engine][importer]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-cache-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  std::filesystem::create_directories(staged->cache_path.parent_path());
  {
    std::ofstream out(staged->cache_path);
    out << "{not json}";
  }
  auto malformed = im::run(root / "repo", home, true);
  REQUIRE_FALSE(malformed.has_value());
  CHECK(malformed.error() == im::error::invalid_input);

  {
    std::ofstream out(staged->cache_path);
    out << R"({"schema_version":1,"fingerprint":"other","anchor_title":"x","provenance":"p","phases":[],"forward_specs":[{"slug":"a"},{"slug":"b"},{"slug":"c"}]})";
  }
  auto mismatched = im::run(root / "repo", home, true);
  REQUIRE_FALSE(mismatched.has_value());
  CHECK(mismatched.error() == im::error::invalid_input);
  std::filesystem::remove_all(root);
}

// --- task 6406: negative-path cover for the untrusted cache envelope --------
//
// `cache_anchor` (importer.cpp:61-63) declares the interpretation cache an
// UNTRUSTED hand-off boundary: it is written out-of-process by a vendor
// skill, not by this binary. Three of its envelope checks -- the
// `forward_specs` count bound, the non-empty `anchor_title`, and the
// non-empty `provenance` -- could each be deleted outright with the suite
// still green (measured by permissive mutation, task 6106 blind review).
// The logic was right; nothing asserted the rejection.
//
// Each case below pairs a rejection with the POSITIVE CONTROL that differs
// only in the field under test, so a rejection for some unrelated reason
// cannot masquerade as cover.

namespace {

/// @brief A cache envelope carrying the real fingerprint, parameterised on
/// exactly the three fields task 6406 leaves uncovered.
auto envelope(std::string_view fingerprint, std::size_t spec_count, std::string_view title, std::string_view provenance)
    -> std::string {
  std::string specs;
  for (std::size_t i = 0; i < spec_count; ++i) {
    if (i > 0)
      specs += ",";
    specs += std::format(R"({{"slug":"spec-{}"}})", i);
  }
  return std::format(R"({{"schema_version":1,"fingerprint":"{}","anchor_title":"{}","provenance":"{}",)"
                     R"("phases":[],"forward_specs":[{}]}})",
                     fingerprint, title, provenance, specs);
}

/// @brief Overwrite the staged cache and re-run staging over it.
auto stage_and_run(const std::filesystem::path& repo, const std::filesystem::path& home, const std::filesystem::path& cache,
                   std::string_view body) -> std::expected<im::outcome, im::error> {
  std::ofstream out(cache);
  out << body;
  out.close();
  return im::run(repo, home, true);
}

} // namespace

TEST_CASE("the untrusted cache envelope is rejected outside its documented bounds", "[engine][importer][6406]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-envelope-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  auto const repo        = root / "repo";
  auto const cache       = staged->cache_path;
  auto const fingerprint = staged->request_.fingerprint;
  std::filesystem::create_directories(cache.parent_path());

  SECTION("forward_specs count is bounded at BOTH ends") {
    // The positive control first: three specs is the low bound and must be
    // ACCEPTED, so a rejection below cannot be blamed on the envelope shape.
    auto const three = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "x", "p"));
    REQUIRE(three.has_value());
    CHECK(three->mode_ == im::outcome::mode::cache_hit);
    auto const five = stage_and_run(repo, home, cache, envelope(fingerprint, 5, "x", "p"));
    REQUIRE(five.has_value());
    CHECK(five->mode_ == im::outcome::mode::cache_hit);

    // Both ends, not one: a bound written as a single comparison passes a
    // one-sided fixture.
    auto const two = stage_and_run(repo, home, cache, envelope(fingerprint, 2, "x", "p"));
    REQUIRE_FALSE(two.has_value());
    CHECK(two.error() == im::error::invalid_input);
    auto const six = stage_and_run(repo, home, cache, envelope(fingerprint, 6, "x", "p"));
    REQUIRE_FALSE(six.has_value());
    CHECK(six.error() == im::error::invalid_input);
  }

  SECTION("anchor_title must be non-empty") {
    auto const present = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "a title", "p"));
    REQUIRE(present.has_value());
    CHECK(present->mode_ == im::outcome::mode::cache_hit);

    auto const empty = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "", "p"));
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == im::error::invalid_input);
  }

  SECTION("provenance must be non-empty") {
    auto const present = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "x", "vendor-skill"));
    REQUIRE(present.has_value());
    CHECK(present->mode_ == im::outcome::mode::cache_hit);

    auto const empty = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "x", ""));
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == im::error::invalid_input);
  }

  std::filesystem::remove_all(root);
}
