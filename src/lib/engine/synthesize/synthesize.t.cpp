// Filesystem/cache tests for the synthesis handoff boundary. Every path is
// beneath a per-test temp arena; the real operator home is unreachable.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.synthesize;

namespace synth = planar::engine::synthesize;

namespace {
auto arena(std::string_view tag) -> std::filesystem::path {
  auto path = std::filesystem::temp_directory_path() /
              std::format("planar-synthesize-{}-{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(path / "repo" / "src");
  return path;
}
auto write(const std::filesystem::path& path, std::string_view body) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  REQUIRE(out.good());
}
auto read(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{in}, {}};
}
auto no_env(std::string_view) -> std::optional<std::string> {
  return std::nullopt;
}
} // namespace

TEST_CASE("synthesize stages the canonical request without touching a database", "[engine][synthesize][pending]") {
  auto const root = arena("pending");
  write(root / "repo" / "README.md", "# Probe Project\n\nAn oracle synthesis probe.\n");
  write(root / "repo" / "AGENTS.md", "# Guide\n\nKeep the probe hermetic.\n");
  write(root / "repo" / "src" / "main.zig", "pub fn main() void {}\n");

  auto result = synth::run(root / "repo", root / "home", {}, no_env);
  REQUIRE(result.has_value());
  CHECK(result->mode_ == synth::mode::pending);
  CHECK(result->provider_ == synth::provider::shell);
  CHECK(result->request_.fingerprint == "05f3394dc1d7f09774bafd7c98385492493f8e7938e589987e0e2fb1ebc0ada5");
  CHECK(result->request_.docs.size() == 1);
  CHECK(result->request_.guide_files.size() == 1);
  CHECK(result->request_.tree_summary == std::vector<std::string>{"AGENTS.md", "README.md", "src/"});
  CHECK(result->request_.areas.size() == 1);
  CHECK(result->request_.areas.front().path == "src/main.zig");
  CHECK(result->request_.greenfield == false);
  auto const pending = read(result->pending_path);
  CHECK(pending.starts_with("{\"schema_version\":1,\"repo_slug\":\"repo\""));
  CHECK(pending.contains("\"guide_files\":{\"AGENTS.md\":"));
  CHECK(pending.contains("\"signal_strength\":0.2"));
  CHECK_FALSE(std::filesystem::exists(root / "planar.db"));
  std::filesystem::remove_all(root);
}

TEST_CASE("synthesize apply without a cache refuses before staging pending state", "[engine][synthesize][apply][refusal]") {
  auto const root = arena("apply-missing");
  write(root / "repo" / "README.md", "# No cache\n");
  auto result = synth::run(root / "repo", root / "home", {.apply = true}, no_env);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == synth::error::not_found);
  CHECK_FALSE(std::filesystem::exists(root / "home" / "cache" / "bootstrap-synthesis" / "repo" / "_pending.json"));
  std::filesystem::remove_all(root);
}

TEST_CASE("synthesize validates the complete cache envelope before exposing a cache hit", "[engine][synthesize][cache]") {
  auto const root = arena("cache");
  write(root / "repo" / "README.md", "# Cache\n");
  auto staged = synth::run(root / "repo", root / "home", {.treat_as_greenfield = true}, no_env);
  REQUIRE(staged.has_value());
  // Optional arrays and task fields are omitted deliberately: Zig decodes
  // them to empty/default values, so the C++ validator must do the same.
  auto valid = std::format(
      R"({{"schema_version":1,"fingerprint":"{}","synthesized":true,"anchor_title":"Anchor","phases":[{{"slug":"phase","status":"draft"}}],"forward_specs":[{{"slug":"a"}},{{"slug":"b"}},{{"slug":"c"}}],"provenance":"fixture"}})",
      staged->request_.fingerprint);
  write(staged->cache_path, valid);
  auto hit = synth::run(root / "repo", root / "home", {.treat_as_greenfield = true}, no_env);
  REQUIRE(hit.has_value());
  CHECK(hit->mode_ == synth::mode::cache_hit);
  REQUIRE(hit->result.has_value());

  // A plausible-looking cache for another request must fail closed.
  auto mismatched = valid;
  auto at         = mismatched.find(staged->request_.fingerprint);
  REQUIRE(at != std::string::npos);
  mismatched.replace(at, staged->request_.fingerprint.size(), std::string(64, '0'));
  write(staged->cache_path, mismatched);
  auto rejected = synth::run(root / "repo", root / "home", {.treat_as_greenfield = true}, no_env);
  REQUIRE_FALSE(rejected.has_value());
  CHECK(rejected.error() == synth::error::invalid_input);
  std::filesystem::remove_all(root);
}

TEST_CASE("synthesize refuses contradictory modes layouts and providers", "[engine][synthesize][flags]") {
  auto const root = arena("flags");
  CHECK(synth::run(root / "repo", root / "home", {.treat_as_greenfield = true, .treat_as_nongreenfield = true}, no_env).error() ==
        synth::error::invalid_input);
  CHECK(synth::run(root / "repo", root / "home", {.code_layout = "rust"}, no_env).error() == synth::error::invalid_input);
  auto bad_env = [](std::string_view key) -> std::optional<std::string> {
    return key == "PLANAR_LLM_PROVIDER" ? std::optional<std::string>{"azure"} : std::nullopt;
  };
  CHECK(synth::run(root / "repo", root / "home", {}, bad_env).error() == synth::error::invalid_input);
  std::filesystem::remove_all(root);
}
