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
