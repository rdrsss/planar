// @file installed_surface.t.cpp
// @brief Tests for `planar.installed_surface` (plan 996, task 6357).
//
// Every filesystem-touching test operates inside a `scratch_root`, a
// uniquely-named directory under `temp_directory_path()` removed on
// destruction. No real `$HOME`/`$PLANAR_HOME`/`$CODEX_HOME` is ever read —
// `options` takes explicit paths, so there is no code path by which a test
// could reach the operator's real vendor directories.
//
// Oracle provenance: zig/src/engine/installedsurface.zig's four `test`
// blocks ("installer vendors are accepted and mapped to their destination
// roots", "status classifies managed copy and unmanaged extension",
// "missing manifest is one bootstrap result", "legacy invalid and
// unsupported manifests stay aggregate bootstrap states") are the source of
// every fixture shape below.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.installed_surface;

namespace is_ = planar::installed_surface;

namespace {

struct scratch_root {
  std::filesystem::path root_;

  scratch_root()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_installed_surface_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(root_);
  }
  scratch_root(const scratch_root&)            = delete;
  scratch_root& operator=(const scratch_root&) = delete;
  ~scratch_root() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] auto planar_home() const -> std::filesystem::path {
    return root_ / ".planar";
  }
  [[nodiscard]] auto codex_home() const -> std::filesystem::path {
    return root_ / ".codex";
  }

  auto write(const std::filesystem::path& path, std::string_view content) const -> void {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
};

} // namespace

TEST_CASE("installer vendors are accepted and mapped to their destination roots", "[installed_surface]") {
  REQUIRE(is_::supported_vendors.size() == 4);
  struct {
    std::string_view vendor;
    std::string_view skill_suffix;
  } expected[] = {
      {"claude", ".claude/commands"},
      {"codex", ".codex/skills"},
      {"copilot", ".copilot/skills"},
      {"gemini", ".gemini/antigravity-cli/skills"},
  };
  for (auto const& entry : expected) {
    CAPTURE(entry.vendor);
    REQUIRE(std::ranges::find(is_::supported_vendors, entry.vendor) != is_::supported_vendors.end());
  }
}

TEST_CASE("missing manifest is one bootstrap result", "[installed_surface]") {
  scratch_root home;
  auto         result = is_::status(is_::options{
      .planar_home = home.planar_home().string(),
      .home        = home.root_.string(),
      .codex_home  = home.codex_home().string(),
  });
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::missing);
  CHECK(result->projections.empty());
  REQUIRE(result->repair_command.has_value());
  CHECK(result->repair_command->starts_with("./install.sh --prefix"));
  CHECK(result->summary.unselected_vendors == is_::supported_vendors.size());
}

TEST_CASE("legacy invalid and unsupported manifests stay aggregate bootstrap states", "[installed_surface]") {
  scratch_root home;
  std::filesystem::create_directories(home.planar_home());
  home.write(home.planar_home() / ".planar-install", "planar-install 1\n");

  {
    auto legacy = is_::status(is_::options{
        .planar_home = home.planar_home().string(),
        .home        = home.root_.string(),
        .codex_home  = home.root_.string(),
    });
    REQUIRE(legacy.has_value());
    CHECK(legacy->manifest_status == is_::manifest_state::legacy);
    CHECK(legacy->projections.empty());
  }

  home.write(home.planar_home() / "install-manifest.json", "not json\n");
  {
    auto invalid = is_::status(is_::options{
        .planar_home = home.planar_home().string(),
        .home        = home.root_.string(),
        .codex_home  = home.root_.string(),
    });
    REQUIRE(invalid.has_value());
    CHECK(invalid->manifest_status == is_::manifest_state::invalid);
  }

  home.write(home.planar_home() / "install-manifest.json", "{\"version\":2}\n");
  {
    auto unsupported = is_::status(is_::options{
        .planar_home = home.planar_home().string(),
        .home        = home.root_.string(),
        .codex_home  = home.root_.string(),
    });
    REQUIRE(unsupported.has_value());
    CHECK(unsupported->manifest_status == is_::manifest_state::unsupported);
  }
}

TEST_CASE("status classifies managed copy and unmanaged extension", "[installed_surface]") {
  scratch_root home;
  const auto   staged    = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed = home.codex_home() / "skills" / "pl-a" / "SKILL.md";
  const auto   unmanaged = home.codex_home() / "skills" / "mine" / "SKILL.md";

  // Manifest schema still carries source_digest/projection_digest, but
  // classification no longer reads or compares them against file content —
  // freshness is plain byte equality now (plan 918 M5). Empty digests are
  // also a valid shape (scriptorium-rendered projections carry none).
  constexpr std::string_view digest            = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr std::string_view projection_digest = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  constexpr std::string_view body              = "---\n---\nbody\n";
  home.write(staged, body);
  home.write(installed, body);
  home.write(unmanaged, "personal\n");

  const auto manifest =
      std::format("{{\"version\":1,\"build_id\":\"test\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"{}\",\"projection_digest\":\"{}\"}}]}}",
                  staged.string(), installed.string(), digest, projection_digest);
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(),
      .home        = home.root_.string(),
      .codex_home  = home.codex_home().string(),
      .vendor      = "codex",
  });
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::current);
  CHECK(result->summary.fresh == 1);
  CHECK(result->summary.unmanaged == 1);
}

// ---- break-probe-driven coverage: branches the oracle's own test set does
// not exercise but this port's non-trivial arms need pinned. ----

TEST_CASE("invalid_input refuses on any empty home", "[installed_surface]") {
  CHECK_FALSE(is_::status(is_::options{.planar_home = "", .home = "/tmp", .codex_home = "/tmp"}).has_value());
  CHECK_FALSE(is_::status(is_::options{.planar_home = "/tmp", .home = "", .codex_home = "/tmp"}).has_value());
  CHECK_FALSE(is_::status(is_::options{.planar_home = "/tmp", .home = "/tmp", .codex_home = ""}).has_value());
}

TEST_CASE("invalid_vendor refuses an unknown vendor filter", "[installed_surface]") {
  auto result = is_::status(is_::options{.planar_home = "/tmp", .home = "/tmp", .codex_home = "/tmp", .vendor = "not-a-vendor"});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == is_::status_error::invalid_vendor);
}

TEST_CASE("a stale row misses its installed file entirely", "[installed_surface]") {
  scratch_root home;
  const auto   staged    = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed = home.codex_home() / "skills" / "pl-a" / "SKILL.md";
  home.write(staged, "content\n");
  // installed is never written.

  const auto manifest =
      std::format("{{\"version\":1,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::missing);
  CHECK(result->summary.missing == 1);
  REQUIRE(result->repair_command.has_value());
}

TEST_CASE("a row whose staged source is unavailable stays stale, not missing", "[installed_surface]") {
  scratch_root home;
  const auto   staged    = home.planar_home() / "codex-skills" / "gone" / "SKILL.md"; // never written
  const auto   installed = home.codex_home() / "skills" / "gone" / "SKILL.md";
  home.write(installed, "content\n");

  const auto manifest =
      std::format("{{\"version\":1,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"gone\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::stale);
  CHECK(result->projections[0].reason == "staged projection is unavailable");
}

TEST_CASE("a symlink install_kind row with the wrong link target is stale", "[installed_surface]") {
  scratch_root home;
  const auto   staged        = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   other_staged  = home.planar_home() / "codex-skills" / "other" / "SKILL.md";
  const auto   installed_dir = home.codex_home() / "skills" / "pl-a";
  const auto   installed     = installed_dir / "SKILL.md";
  home.write(staged, "content\n");
  home.write(other_staged, "content\n");
  std::filesystem::create_directories(installed_dir);
  std::error_code ec;
  std::filesystem::create_symlink(other_staged, installed, ec);
  REQUIRE_FALSE(ec);

  const auto manifest =
      std::format("{{\"version\":1,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"link\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::stale);
  CHECK(result->projections[0].reason == "managed link target differs from the install manifest");
}

TEST_CASE("a symlink install_kind row whose link target matches the manifest is fresh", "[installed_surface]") {
  // The symmetric direction of the case above: until this iteration, no
  // fixture exercised a CORRECT `link` row, so a mutation that made this
  // branch over-eager (always `stale` regardless of match) would have
  // survived — the suite only ever proved "wrong target -> stale", never
  // "right target -> fresh".
  scratch_root home;
  const auto   staged        = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed_dir = home.codex_home() / "skills" / "pl-a";
  const auto   installed     = installed_dir / "SKILL.md";
  home.write(staged, "content\n");
  std::filesystem::create_directories(installed_dir);
  std::error_code ec;
  std::filesystem::create_symlink(staged, installed, ec);
  REQUIRE_FALSE(ec);

  const auto manifest =
      std::format("{{\"version\":1,\"build_id\":\"t\",\"install_mode\":\"link\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"link\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::fresh);
  CHECK(result->projections[0].reason == "staged projection and installed projection agree");
  CHECK(result->summary.fresh == 1);
  CHECK_FALSE(result->projections[0].repair_command.has_value());
}

TEST_CASE("an invalid manifest structure (duplicate vendor) is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"build_id":"t","install_mode":"copy","vendors":["codex","codex"],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest with a duplicate top-level key is rejected like std.json", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"version":1,"build_id":"t","install_mode":"copy","vendors":[],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("an unmanaged agent (flat file) is discovered and named without its suffix", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"build_id":"t","install_mode":"copy","vendors":["claude"],"projections":[]})");
  home.write(home.root_ / ".claude" / "agents" / "mine.md", "content\n");

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "claude"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::unmanaged);
  CHECK(result->projections[0].name == "mine");
  CHECK(result->summary.unmanaged == 1);
}

TEST_CASE("an unselected vendor's destination is never walked for unmanaged entries", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":[]})");
  // claude is never selected, but has an unmanaged-looking file sitting
  // under its would-be destination.
  home.write(home.root_ / ".claude" / "agents" / "stray.md", "content\n");

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->projections.empty());
  CHECK(result->summary.unmanaged == 0);
}

// ---- iteration-2 review findings: the PERMISSIVE direction of a check is
// its dangerous direction, and the original fixture set only asserted the
// restrictive one for these two. See installed_surface.cpp's classify_row
// (byte comparison) and only_keys (unknown-field rejection). ----

TEST_CASE("a copy row whose installed bytes differ from staged is stale, never silently fresh", "[installed_surface]") {
  // The safety-critical direction: `planar health` exists to catch exactly
  // this — a managed install that has drifted from its staged source. A
  // classifier that reports this row `fresh` is a health check that lies
  // about a stale surface being healthy.
  scratch_root home;
  const auto   staged    = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed = home.codex_home() / "skills" / "pl-a" / "SKILL.md";
  home.write(staged, "staged content\n");
  home.write(installed, "DIFFERENT installed content\n");

  const auto manifest =
      std::format("{{\"version\":1,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::stale);
  CHECK(result->projections[0].reason == "managed installed bytes differ from the staged projection");
  CHECK(result->summary.stale == 1);
  CHECK(result->summary.fresh == 0);
  REQUIRE(result->projections[0].repair_command.has_value());
}

TEST_CASE("a projection row with an unrecognized key is rejected as an invalid manifest", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":"","unexpected_field":"surprise"}]})");

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
  CHECK(result->projections.empty());
}

TEST_CASE("a top-level manifest object with an unrecognized key is rejected as invalid", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"build_id":"t","install_mode":"copy","vendors":[],"projections":[],)"
             R"("unexpected_top_level_field":true})");

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}
