// @file templates.t.cpp
// @brief Unit tests for `planar.engine.config.templates` /
// `planar.engine.config.templates_embed` (plan 996, task 6032). Exercises
// the real #embed-generated set (deterministic (system, kind) enumeration
// order — the anti-glob-order mandate, tech-spec § "Embedded migrations
// and templates") plus the three-level disk-then-embedded resolution
// chain, mirroring the scenarios
// zig/src/engine/templates/loader.zig's own test suite pins: embedded
// resolution with no root, an operator override on disk winning over the
// embedded default, and the empty/absent-root cases.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.config.templates;
import planar.engine.config.templates_embed;

using planar::engine::config::embedded_templates;
using planar::engine::config::list_disk_entries;
using planar::engine::config::list_embedded_entries;
using planar::engine::config::load_template;
using planar::engine::config::template_error;
using planar::engine::config::template_source;

namespace {

/// @brief A unique scratch directory under the system temp directory,
/// removed recursively when the guard goes out of scope. Mirrors the
/// scratch_db_path pattern used throughout src/lib/db and
/// src/lib/engine/identity's test files.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_templates_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
};

void write_file(const std::filesystem::path& p, std::string_view content) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

} // namespace

TEST_CASE("embedded_templates: non-empty and every entry has a real system/kind/body", "[templates]") {
  auto all = embedded_templates();
  REQUIRE(all.size() >= 4); // github-issues has 5 kinds, jira has 4, github-projects has 1.
  for (const auto& e : all) {
    CHECK_FALSE(e.system.empty());
    CHECK_FALSE(e.kind.empty());
    CHECK_FALSE(e.body.empty());
  }
}

TEST_CASE("embedded_templates: strictly ascending (system, kind) order — the anti-glob-order mandate", "[templates]") {
  auto all = embedded_templates();
  for (std::size_t i = 1; i < all.size(); ++i) {
    const auto prev = std::make_pair(all[i - 1].system, all[i - 1].kind);
    const auto curr = std::make_pair(all[i].system, all[i].kind);
    CHECK(prev < curr);
  }
}

TEST_CASE("load_template: resolves the embedded default when no root is given", "[templates]") {
  auto result = load_template("default", "github-issues", "issue", "");
  REQUIRE(result.has_value());
  CHECK(result->system == "github-issues");
  CHECK(result->kind == "issue");
  CHECK(result->source_ == template_source::embedded);
  CHECK(result->path == "embedded:github-issues/issue.json");
  CHECK_FALSE(result->raw.empty());
}

TEST_CASE("load_template: unknown kind is not_found even with the embedded fallback", "[templates]") {
  auto result = load_template("default", "github-issues", "no-such-kind", "");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == template_error::not_found);
}

TEST_CASE("load_template: an operator's default-set disk override wins over the embedded default", "[templates]") {
  scratch_dir root;
  write_file(root.path_ / "default" / "github-issues" / "issue.json", R"({"overridden": true})");

  auto result = load_template("default", "github-issues", "issue", root.path_.string());
  REQUIRE(result.has_value());
  CHECK(result->source_ == template_source::disk);
  CHECK(result->raw == R"({"overridden": true})");
}

TEST_CASE("load_template: a named-set disk override wins over both the default-set disk file and the embedded default",
          "[templates]") {
  scratch_dir root;
  write_file(root.path_ / "default" / "github-issues" / "issue.json", R"({"from": "default-set"})");
  write_file(root.path_ / "acme" / "github-issues" / "issue.json", R"({"from": "acme-set"})");

  auto result = load_template("acme", "github-issues", "issue", root.path_.string());
  REQUIRE(result.has_value());
  CHECK(result->source_ == template_source::disk);
  CHECK(result->raw == R"({"from": "acme-set"})");
  CHECK(result->set_name == "acme");
}

TEST_CASE("load_template: a named set with no override file falls through to the default-set disk file", "[templates]") {
  scratch_dir root;
  write_file(root.path_ / "default" / "github-issues" / "issue.json", R"({"from": "default-set"})");

  auto result = load_template("acme", "github-issues", "issue", root.path_.string());
  REQUIRE(result.has_value());
  CHECK(result->source_ == template_source::disk);
  CHECK(result->raw == R"({"from": "default-set"})");
  CHECK(result->set_name == "default");
}

TEST_CASE("load_template: a nonexistent root directory falls through cleanly to the embedded default", "[templates]") {
  auto result = load_template("default", "jira", "epic", "/nonexistent/planar/templates/root");
  REQUIRE(result.has_value());
  CHECK(result->source_ == template_source::embedded);
}

TEST_CASE("list_embedded_entries: one entry per embedded template, all set_name=\"default\"", "[templates]") {
  auto entries = list_embedded_entries();
  CHECK(entries.size() == embedded_templates().size());
  for (const auto& e : entries) {
    CHECK(e.set_name == "default");
    CHECK(e.source_ == template_source::embedded);
    CHECK(e.raw.empty());
  }
}

TEST_CASE("list_disk_entries: a nonexistent root returns an empty result", "[templates]") {
  auto entries = list_disk_entries("/nonexistent/planar/templates/root");
  CHECK(entries.empty());
}

TEST_CASE("list_disk_entries: an empty root string returns an empty result", "[templates]") {
  auto entries = list_disk_entries("");
  CHECK(entries.empty());
}

TEST_CASE("list_disk_entries: enumerates every set/system/kind triple on disk, sorted", "[templates]") {
  scratch_dir root;
  write_file(root.path_ / "acme" / "jira" / "story.json", "{}");
  write_file(root.path_ / "acme" / "github-issues" / "issue.json", "{}");
  write_file(root.path_ / "default" / "jira" / "epic.json", "{}");
  // A non-.json file must be ignored.
  write_file(root.path_ / "acme" / "jira" / "README.md", "not a template");

  auto entries = list_disk_entries(root.path_.string());
  REQUIRE(entries.size() == 3);
  CHECK(std::ranges::is_sorted(entries, {}, [](const auto& e) { return std::tie(e.set_name, e.system, e.kind); }));
  CHECK(entries.front().set_name == "acme");
}
