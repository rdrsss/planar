// @file extract.t.cpp
// @brief Disk-extraction tests for `planar.engine.templates.extract`
// (plan 996, task 6190).
//
// HOME SAFETY. This file WRITES FILES, so it is the one place in this
// bucket where a wrong root would edit the developer's machine. The
// protection is structural rather than conventional:
// `extract_defaults` takes an explicit absolute root and calls no
// `std::getenv`, so it has no way to find a real home at all. Every test
// below passes a fresh `std::filesystem::temp_directory_path()` subtree and
// removes it on the way out.
//
// The embedded set is a PARAMETER for the same reason it is a parameter in
// production (D15 forbids `engine_templates -> engine_config`), and it pays
// off here: these tests drive three synthetic entries rather than the real
// ten-file set, so a change to `templates/defaults/` cannot make them fail.
//
// ORACLE PROVENANCE. The idempotency and `--force` behaviours were
// captured by running `templates init` three times in a pinned arena:
//   1st -> `templates init: wrote 10 file(s)` + ten paths
//   2nd -> `templates init: nothing to do (all templates already present)`
//   3rd, with --force -> the SAME nothing-to-do sentence

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.templates.extract;

namespace tpl = planar::engine::templates;

namespace {

/// @brief A scratch directory that removes itself.
///
/// Not a shared fixture path: concurrent Catch2 cases would collide, and a
/// collision here means one test deleting another's files mid-run.
class scratch {
private:
  std::filesystem::path _dir;

public:
  /// @brief Create a uniquely-named scratch root under the system temp dir.
  /// @param tag A discriminator so a failure names its own case.
  explicit scratch(std::string_view tag) {
    _dir = std::filesystem::temp_directory_path() /
           std::format("planar_tmpl_extract_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(_dir);
  }
  scratch(const scratch&)            = delete;
  scratch& operator=(const scratch&) = delete;
  scratch(scratch&&)                 = delete;
  scratch& operator=(scratch&&)      = delete;
  /// @brief Remove the scratch tree.
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
  }
  /// @brief The scratch root.
  /// @return The path.
  [[nodiscard]] auto dir() const -> const std::filesystem::path& {
    return _dir;
  }
};

/// @brief Three synthetic templates spanning two systems.
/// @return The set.
auto fixture_set() -> std::vector<tpl::embedded_file> {
  return {{.system = "jira", .kind = "epic", .body = R"({"k":"epic"})"},
          {.system = "jira", .kind = "story", .body = R"({"k":"story"})"},
          {.system = "github-issues", .kind = "issue", .body = R"({"k":"issue"})"}};
}

/// @brief Read a file whole.
/// @param path The file.
/// @return Its bytes, or nullopt when absent.
auto read_all(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("extract_defaults writes every entry under <root>/default/<system>/<kind>.json", "[templates][extract]") {
  scratch const s{"writes"};
  auto const    created = tpl::extract_defaults(s.dir(), fixture_set());
  REQUIRE(created.has_value());
  REQUIRE(created->size() == 3);

  // The PATH SHAPE is contract — `templates init`'s text output prints
  // these verbatim and an operator's `templates list` finds them by
  // walking exactly this hierarchy.
  CHECK(std::filesystem::exists(s.dir() / "default" / "jira" / "epic.json"));
  CHECK(std::filesystem::exists(s.dir() / "default" / "jira" / "story.json"));
  CHECK(std::filesystem::exists(s.dir() / "default" / "github-issues" / "issue.json"));

  // Bodies are written VERBATIM, not re-encoded.
  CHECK(read_all(s.dir() / "default" / "jira" / "epic.json") == R"({"k":"epic"})");
}

TEST_CASE("extract_defaults returns the created paths in the SET's order", "[templates][extract]") {
  // `templates init`'s text output lists them in this order, so it is
  // observable rather than incidental.
  scratch const s{"order"};
  auto const    created = tpl::extract_defaults(s.dir(), fixture_set());
  REQUIRE(created.has_value());
  REQUIRE(created->size() == 3);
  CHECK((*created)[0].ends_with("jira/epic.json"));
  CHECK((*created)[1].ends_with("jira/story.json"));
  CHECK((*created)[2].ends_with("github-issues/issue.json"));
}

TEST_CASE("extract_defaults is IDEMPOTENT — a second run creates nothing", "[templates][extract]") {
  // Oracle: the second `templates init` printed the nothing-to-do
  // sentence. An empty return is what drives that branch.
  scratch const s{"idempotent"};
  REQUIRE(tpl::extract_defaults(s.dir(), fixture_set())->size() == 3);
  auto const second = tpl::extract_defaults(s.dir(), fixture_set());
  REQUIRE(second.has_value());
  CHECK(second->empty());
}

TEST_CASE("extract_defaults NEVER overwrites an existing file", "[templates][extract]") {
  // The load-bearing half of idempotency, and the one an "empty result"
  // assertion alone would not catch: a run that rewrote every file and
  // reported none created would pass that check and silently destroy an
  // operator's edits.
  scratch const s{"nooverwrite"};
  REQUIRE(tpl::extract_defaults(s.dir(), fixture_set()).has_value());

  auto const target = s.dir() / "default" / "jira" / "epic.json";
  {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << "OPERATOR EDIT";
  }
  auto const second = tpl::extract_defaults(s.dir(), fixture_set());
  REQUIRE(second.has_value());
  CHECK(second->empty());
  CHECK(read_all(target) == "OPERATOR EDIT");
}

TEST_CASE("extract_defaults fills in only the MISSING entries of a partial tree", "[templates][extract]") {
  scratch const s{"partial"};
  REQUIRE(tpl::extract_defaults(s.dir(), fixture_set()).has_value());
  std::filesystem::remove(s.dir() / "default" / "jira" / "story.json");

  auto const again = tpl::extract_defaults(s.dir(), fixture_set());
  REQUIRE(again.has_value());
  REQUIRE(again->size() == 1);
  CHECK((*again)[0].ends_with("jira/story.json"));
}

TEST_CASE("extract_defaults creates intermediate directories that do not exist", "[templates][extract]") {
  scratch const s{"mkdirs"};
  auto const    deep = s.dir() / "a" / "b" / "c";
  REQUIRE(tpl::extract_defaults(deep, fixture_set()).has_value());
  CHECK(std::filesystem::exists(deep / "default" / "jira" / "epic.json"));
}

TEST_CASE("extract_defaults refuses an EMPTY root", "[templates][extract]") {
  // zig returns `error.InvalidInput`. Silently treating "" as the CWD
  // would scatter a `default/` tree wherever the operator happened to be.
  auto const result = tpl::extract_defaults(std::filesystem::path{}, fixture_set());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == tpl::extract_error::invalid_input);
}

TEST_CASE("extract_defaults on an EMPTY set creates the default dir and nothing else", "[templates][extract]") {
  scratch const s{"emptyset"};
  auto const    created = tpl::extract_defaults(s.dir(), {});
  REQUIRE(created.has_value());
  CHECK(created->empty());
  CHECK(std::filesystem::exists(s.dir() / "default"));
}

TEST_CASE("extract_defaults treats a DIRECTORY in a template's place as present", "[templates][extract]") {
  // zig probes with `access`, which succeeds for a directory, so the entry
  // is skipped rather than clobbered or reported. Reproduced.
  scratch const s{"dirinplace"};
  std::filesystem::create_directories(s.dir() / "default" / "jira" / "epic.json");
  auto const created = tpl::extract_defaults(s.dir(), fixture_set());
  REQUIRE(created.has_value());
  REQUIRE(created->size() == 2);
  CHECK(std::filesystem::is_directory(s.dir() / "default" / "jira" / "epic.json"));
}
