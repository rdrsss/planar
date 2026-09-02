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
  { std::ofstream out(root / "repo" / "README.md"); out << "# Imported title\n"; }
  { std::ofstream out(root / "repo" / "docs" / "guide.md"); out << "body\n"; }

  auto preview = im::run(root / "repo", home, false);
  REQUIRE(preview.has_value());
  CHECK(preview->mode_ == im::outcome::mode::skipped);
  CHECK(preview->request_.anchor_title == "Imported title");
  CHECK(preview->request_.docs_count == 2);
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
