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
#include <sys/stat.h> // chmod — the permission-denied fixture below
#include <unistd.h>   // geteuid — root-skip guard for the same fixture

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

TEST_CASE("the six installer vendors are accepted", "[installed_surface]") {
  REQUIRE(is_::supported_vendors.size() == 6);
  for (std::string_view vendor : {"claude", "codex", "copilot", "gemini", "antigravity", "opencode"}) {
    CAPTURE(vendor);
    CHECK(std::ranges::find(is_::supported_vendors, vendor) != is_::supported_vendors.end());
  }
  CHECK(is_::manifest_version == 2);
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

  // A version 1 manifest (the retired four-vendor layout, or the interim one
  // that kept placements only in `extras`) is `legacy`, never `invalid`: it
  // carries a reinstall repair command and no rows.
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":1,"build_id":"t","install_mode":"copy","vendors":[],"extras":["/x"],"projections":[]})");
  {
    auto old = is_::status(is_::options{
        .planar_home = home.planar_home().string(),
        .home        = home.root_.string(),
        .codex_home  = home.root_.string(),
    });
    REQUIRE(old.has_value());
    CHECK(old->manifest_status == is_::manifest_state::legacy);
    CHECK(old->projections.empty());
    REQUIRE(old->reason.has_value());
    CHECK(old->reason->contains("version 1"));
    REQUIRE(old->repair_command.has_value());
    CHECK(old->summary.unselected_vendors == is_::supported_vendors.size());
  }

  home.write(home.planar_home() / "install-manifest.json", "{\"version\":3}\n");
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
  const auto   unmanaged = home.root_ / ".agents" / "skills" / "planar" / "SKILL.md"; // the shared root

  // Manifest schema still carries source_digest/projection_digest, but
  // classification no longer reads or compares them against file content —
  // freshness is plain byte equality now (plan 918 M5). Empty digests are
  // also a valid shape (projections from older installs carry none).
  constexpr std::string_view digest            = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  constexpr std::string_view projection_digest = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  constexpr std::string_view body              = "---\n---\nbody\n";
  home.write(staged, body);
  home.write(installed, body);
  home.write(unmanaged, "personal\n");

  const auto manifest =
      std::format("{{\"version\":2,\"build_id\":\"test\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"{}\",\"projection_digest\":\"{}\"}}]}}",
                  staged.string(), installed.string(), digest, projection_digest);
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{
      .planar_home    = home.planar_home().string(),
      .home           = home.root_.string(),
      .codex_home     = home.codex_home().string(),
      .vendor         = "codex",
      .codex_home_set = true,
  });
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::current);
  CHECK(result->summary.fresh == 1);
  CHECK(result->summary.unmanaged == 1);
  // Top-level `repair_command` gate: stale+missing == 0 here (the only
  // managed row is fresh; the second row is unmanaged, which never
  // contributes to stale/missing), so no reinstall should be suggested.
  CHECK_FALSE(result->repair_command.has_value());
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
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
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
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
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
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
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

TEST_CASE("a copy install_kind row whose installed path is unexpectedly a symlink is stale, even with matching bytes",
          "[installed_surface]") {
  // Isolates the `is_link` disjunct of `else if (is_link || *staged !=
  // *installed)` from the byte-mismatch disjunct: the symlink's TARGET
  // content is byte-identical to staged (so `*staged != *installed` is
  // false), but a `copy` row that materialized as a symlink is still a
  // shape mismatch the operator did not ask for.
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
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
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
}

TEST_CASE("a link install_kind row whose installed path is a plain file, not a symlink, is stale", "[installed_surface]") {
  // Pins a real, previously-untested `link` shape (a plain file where a
  // symlink was expected) — but does NOT isolate the `!is_link` disjunct
  // of `if (!is_link || link_target != row.staged_path)` from its sibling.
  // Verified (iteration 4 review, empirically): `link_target` is a
  // default-constructed empty path whenever `is_link` is false — by
  // construction, only the `is_symlink(...) == true` branch of the
  // ternary that computes it can ever produce a non-empty value — so
  // `link_target.string() != row.staged_path` is ALWAYS true whenever
  // `!is_link` is true (an empty string can never equal a `staged_path`,
  // which `valid_manifest` already rejects as empty). The `!is_link`
  // disjunct is a genuine EQUIVALENT MUTANT here: no manifest can make it
  // fire without the target-mismatch disjunct also firing. Confirmed by
  // re-probing after this fixture landed — still SURVIVOR.
  scratch_root home;
  const auto   staged        = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed_dir = home.codex_home() / "skills" / "pl-a";
  const auto   installed     = installed_dir / "SKILL.md";
  home.write(staged, "content\n");
  home.write(installed, "content\n"); // a plain copy, not a symlink

  const auto manifest =
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"link\",\"vendors\":[\"codex\"],\"projections\":"
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
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"link\",\"vendors\":[\"codex\"],\"projections\":"
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
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex","codex"],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

// `valid_manifest`'s per-projection identity checks (:288-296) sit AFTER
// the per-row structural checks in the same loop and were unexercised in
// either direction until iteration 3 review — the "duplicate vendor" case
// above covers a DIFFERENT, earlier check (duplicate entries in the
// manifest's own `vendors` list), which is exactly the kind of near-miss
// that reads as coverage without being it. Each identity rule gets its own
// case, isolated from the other by construction: the installed_path case
// uses distinct (vendor,kind,name) so ONLY the path collides, and the
// identity case uses distinct installed_path so ONLY (vendor,kind,name)
// collides.

TEST_CASE("two projection rows sharing the same installed_path are rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/staged/a","installed_path":"/installed/shared",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""},)"
             R"({"vendor":"codex","kind":"agent","name":"pl-b","staged_path":"/staged/b","installed_path":"/installed/shared",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("two projection rows sharing the same (vendor, kind, name) are rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/staged/a","installed_path":"/installed/one",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""},)"
             R"({"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/staged/a2","installed_path":"/installed/two",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

// ---- iteration-3 review findings: an exhaustive permissive-mutation sweep
// of the WHOLE module (not just the two gaps named directly) surfaced
// eighteen further checks with no fixture proving either the accept or the
// reject side. Each gets its own case below, following the same principle
// as the earlier fixes: isolate the ONE condition each test targets so a
// mutation to a DIFFERENT nearby check cannot accidentally satisfy it. ----

TEST_CASE("a manifest whose top-level JSON is not an object is rejected as invalid", "[installed_surface]") {
  // `probe_version` and `parse_manifest_doc` each carry their own
  // non-object guard; this fixes both at once since `parse_manifest_doc`'s
  // is unreachable through `status()` unless `probe_version`'s already let
  // a non-object document through.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json", R"([1,2,3])");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest object missing the vendors field entirely is rejected as invalid", "[installed_surface]") {
  // `parse_manifest_doc`'s `vendors == nullptr` clause specifically —
  // distinct from "vendors is not an array" (`vendors->kind != array`)
  // above, which requires the KEY to be present with the wrong type. This
  // is NOT mutation-probed: disabling this clause while "vendors" is
  // absent leaves `vendors` a genuine null pointer, and the next line
  // dereferences it (`vendors->array`) — an actual null-pointer read, not
  // the safe always-empty-member fallback a wrong-TYPE value gets. Pinning
  // the correct, unmutated contract only.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest object missing the projections field entirely is rejected as invalid", "[installed_surface]") {
  // The `projections == nullptr` clause — same null-pointer-dereference
  // risk as the vendors case above if mutation-probed, so left as a
  // correct-behavior pin only.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json", R"({"version":2,"build_id":"t","install_mode":"copy","vendors":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest whose projections field is not an array is rejected as invalid", "[installed_surface]") {
  // `projections->kind != array` — safe to mutation-probe like `vendors`'s
  // equivalent, since a wrong-TYPE `json_value`'s unused `.array` member is
  // always a harmless empty vector, never a dereferenced null/optional.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":[],"projections":"oops"})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest whose vendors field is not an array is rejected as invalid", "[installed_surface]") {
  // Exercises `parse_manifest_doc`'s required-field/type OR-chain — a
  // permissive mutation across the WHOLE chain survived even with a
  // "missing build_id" fixture, because dereferencing the disengaged
  // `std::optional<std::string>` that check exists to prevent is
  // undefined behaviour, and in practice happened to coincide with the
  // (separately, already-tested) empty-build_id rejection rather than
  // proving THIS check. A wrong-TYPE `vendors` has no such escape hatch:
  // `vendors->array` on a non-array `json_value` is always the SAFE empty
  // vector (see json_dom's discriminated-value shape), so a disabled type
  // check here would silently treat a malformed `"vendors":"oops"` as an
  // empty, valid vendor list rather than rejecting it — observable,
  // deterministic, no UB.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":"oops","projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("an extras field that is not an array at all is rejected as invalid", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":[],"extras":"oops","projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("an extras array containing a non-string item is rejected as invalid", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":[],"extras":[1,2],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest with an empty build_id is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"","install_mode":"copy","vendors":[],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest with an install_mode outside copy/link is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"symlink","vendors":[],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest with a negative version number is rejected as invalid, not treated as unsupported", "[installed_surface]") {
  // `as_uint32`'s bounds clause (`value->integer < 0`): a negative JSON
  // integer parses fine as `json_kind::integer` (json_dom's `integer` field
  // is a signed int64), so this is genuinely reachable, not merely
  // defensive. Distinguishing "invalid" from "unsupported" here matters —
  // `probe_version` returning nullopt for an out-of-range value takes the
  // SAME generic-invalid path a missing/non-numeric version does, not the
  // more specific "unsupported" path a valid-but-wrong version (e.g. 2)
  // takes.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json", R"({"version":-1,"build_id":"t","install_mode":"copy"})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a manifest whose vendors list names an unsupported vendor is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["notavendor"],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row missing a required string field entirely is rejected as invalid", "[installed_surface]") {
  // `parse_row`'s 8-way required-field OR-chain (`!vendor || !kind || ...`)
  // is exercised here for the FIRST time — every existing row-shape fixture
  // supplies all eight fields (the only_keys / structural-validity cases
  // add or corrupt one field, never remove one). Deliberately NOT mutation-
  // probed clause-by-clause: unlike `vendors`/`extras` (which fall through
  // to an always-safe, always-empty `.array` member on a type mismatch),
  // every one of these 8 clauses gates a direct `*optional<std::string>`
  // dereference. Disabling any one clause and supplying that exact missing
  // field makes the mutant dereference a disengaged optional — undefined
  // behaviour, not a deterministic "wrong but observable" outcome. A
  // "kill" under such a mutation could be a genuine proof or could be a
  // lucky crash/coincidental garbage value, indistinguishable from outside
  // — the same false-confidence trap iteration 4 review found in the
  // build_id sub-clause of `parse_manifest_doc`'s OR-chain, except there is
  // no type-mismatch escape hatch here to route around it. This case pins
  // the NORMAL, unmutated contract instead.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})"); // "vendor" key omitted
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row naming a vendor absent from the manifest's own vendors list is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"claude","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with an invalid kind is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"toolbox","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with an empty name is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with an empty staged_path is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with an empty installed_path is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with an install_kind outside copy/link is rejected", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"symlink","source_digest":"","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with a digest that is valid hex but the wrong length is rejected", "[installed_surface]") {
  // Isolates `is_digest`'s LENGTH clause (`value.size() != 64`) from its hex-
  // alphabet clause: "abc123" is six characters of valid lowercase hex, so
  // only the length check can reject it. Iteration-4 review finding: the
  // prior single fixture here ("not-a-digest") was 12 chars AND non-hex, so
  // disabling the length clause alone left the hex-alphabet clause to still
  // reject it — the length branch itself had no discriminating case.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"abc123","projection_digest":""}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with a 64-character digest containing a non-hex character is rejected", "[installed_surface]") {
  // Isolates `is_digest`'s HEX-ALPHABET clause from its length clause: this
  // value is exactly 64 characters (built programmatically, not counted by
  // hand, to guarantee it), so only the `all_of(isdigit || a-f)` check can
  // reject it — a single leading 'g' (outside a-f) is the only thing wrong
  // with it.
  scratch_root      home;
  const std::string bad_digest = "g" + std::string(63, 'a');
  REQUIRE(bad_digest.size() == 64);
  home.write(home.planar_home() / "install-manifest.json",
             std::format(R"({{"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
                         R"([{{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
                         R"("install_kind":"copy","source_digest":"{}","projection_digest":""}}]}})",
                         bad_digest));
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a projection row with a malformed projection_digest (source_digest valid) is rejected", "[installed_surface]") {
  // Isolates the SECOND disjunct of `!is_digest(source_digest) ||
  // !is_digest(projection_digest)` — every other digest fixture leaves
  // `projection_digest` empty (valid), so only `source_digest` had ever
  // been proven to reach this OR. A permissive mutation of the
  // `projection_digest` clause alone survived until this fixture.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
             R"([{"vendor":"codex","kind":"skill","name":"pl-a","staged_path":"/x","installed_path":"/y",)"
             R"("install_kind":"copy","source_digest":"","projection_digest":"abc123"}]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("an installed path that is a directory stays stale with its own reason, never fresh or missing",
          "[installed_surface]") {
  scratch_root home;
  const auto   staged        = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed_dir = home.codex_home() / "skills" / "pl-a" / "SKILL.md"; // a DIRECTORY at this exact path
  home.write(staged, "content\n");
  std::filesystem::create_directories(installed_dir);

  const auto manifest =
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed_dir.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::stale);
  CHECK(result->projections[0].reason == "managed installed destination is a directory and cannot be replaced safely");
}

TEST_CASE("a dangling symlink at the installed path is missing, not stale", "[installed_surface]") {
  // The installed path IS a symlink (so `symlink_status` reports neither
  // `not_found` nor `directory`), but its target does not exist, so
  // reading through it fails — the SECOND missing-detection arm in
  // classify_row, reachable only through this exact shape.
  scratch_root home;
  const auto   staged        = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed_dir = home.codex_home() / "skills" / "pl-a";
  const auto   installed     = installed_dir / "SKILL.md";
  home.write(staged, "content\n");
  std::filesystem::create_directories(installed_dir);
  std::error_code ec;
  std::filesystem::create_symlink(home.root_ / "nonexistent-target", installed, ec);
  REQUIRE_FALSE(ec);

  const auto manifest =
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
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
  CHECK(result->projections[0].reason == "managed installed projection is missing");
}

TEST_CASE("a directory-shaped vendor entry with no SKILL.md is never reported unmanaged", "[installed_surface]") {
  // `discover_unmanaged`'s directory_shape existence check: a stray empty
  // subdirectory under a `skills/` root (codex/copilot/gemini all link the
  // whole directory, not a flat file) must not be counted just because a
  // directory with that name exists.
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":[]})");
  std::filesystem::create_directories(home.root_ / ".agents" / "skills" / "planar"); // no SKILL.md inside

  auto result = is_::status(is_::options{.planar_home    = home.planar_home().string(),
                                         .home           = home.root_.string(),
                                         .codex_home     = home.codex_home().string(),
                                         .vendor         = "codex",
                                         .codex_home_set = true});
  REQUIRE(result.has_value());
  CHECK(result->projections.empty());
  CHECK(result->summary.unmanaged == 0);
}

TEST_CASE("the --vendor filter excludes other vendors' projections from a multi-vendor manifest", "[installed_surface]") {
  scratch_root home;
  const auto   codex_staged     = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   codex_installed  = home.codex_home() / "skills" / "pl-a" / "SKILL.md";
  const auto   claude_staged    = home.planar_home() / "claude-commands" / "pl-b.md";
  const auto   claude_installed = home.root_ / ".claude" / "commands" / "pl-b.md";
  home.write(codex_staged, "a\n");
  home.write(codex_installed, "a\n");
  home.write(claude_staged, "b\n");
  home.write(claude_installed, "b\n");

  const auto manifest = std::format(
      "{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\",\"claude\"],\"projections\":"
      "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
      "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}},"
      "{{\"vendor\":\"claude\",\"kind\":\"skill\",\"name\":\"pl-b\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
      "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
      codex_staged.string(), codex_installed.string(), claude_staged.string(), claude_installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].vendor == "codex");
  CHECK(result->projections[0].name == "pl-a");
  // The SAME `--vendor` filter (`opts.vendor && *opts.vendor != vendor`)
  // also gates the vendor-STATUS loop, a separate call site from the one
  // that filters `projections` above — this closes that clause too.
  REQUIRE(result->vendors.size() == 1);
  CHECK(result->vendors[0].vendor == "codex");
}

TEST_CASE("a manifest with a duplicate top-level key is rejected like std.json", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"version":2,"build_id":"t","install_mode":"copy","vendors":[],"projections":[]})");
  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("an unmanaged agent (flat file) is discovered and named without its suffix", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["claude"],"projections":[]})");
  home.write(home.root_ / ".claude" / "agents" / "planar-mine.md", "content\n");
  home.write(home.root_ / ".claude" / "agents" / "mine.md", "the user's own agent\n"); // outside the planar namespace

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "claude"});
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status == is_::state::unmanaged);
  CHECK(result->projections[0].name == "planar-mine");
  CHECK(result->summary.unmanaged == 1);
}

TEST_CASE("an unselected vendor's destination is never walked for unmanaged entries", "[installed_surface]") {
  scratch_root home;
  home.write(home.planar_home() / "install-manifest.json",
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":[]})");
  // claude is never selected, but has an unmanaged-looking file sitting
  // under its would-be destination.
  home.write(home.root_ / ".claude" / "agents" / "planar-stray.md", "content\n");

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
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
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
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":["codex"],"projections":)"
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
             R"({"version":2,"build_id":"t","install_mode":"copy","vendors":[],"projections":[],)"
             R"("unexpected_top_level_field":true})");

  auto result = is_::status(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::invalid);
}

TEST_CASE("a genuinely permission-denied staged file degrades to stale rather than aborting", "[installed_surface][6359]") {
  // Decision 1119 (task 6359): the accepted divergence from the oracle,
  // which propagates any I/O error besides "not found" out of `status()` as
  // a hard failure. This is the one fault-injection case that can exercise
  // it -- `chmod 000` reliably produces EACCES on this platform (measured
  // directly), PROVIDED the process is not root, where permission bits are
  // bypassed entirely and this would pass vacuously. Skip rather than
  // report a false green in that case, matching this project's "no vacuous
  // green" standard.
  if (::geteuid() == 0) {
    SUCCEED("skipped: running as root, chmod 000 would not deny access");
    return;
  }

  scratch_root home;
  const auto   staged    = home.planar_home() / "codex-skills" / "pl-a" / "SKILL.md";
  const auto   installed = home.codex_home() / "skills" / "pl-a" / "SKILL.md";
  home.write(staged, "staged content\n");
  home.write(installed, "installed content\n");
  REQUIRE(::chmod(staged.c_str(), 0) == 0);

  const auto manifest =
      std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[\"codex\"],\"projections\":"
                  "[{{\"vendor\":\"codex\",\"kind\":\"skill\",\"name\":\"pl-a\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                  "\"install_kind\":\"copy\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                  staged.string(), installed.string());
  home.write(home.planar_home() / "install-manifest.json", manifest);

  auto result = is_::status(is_::options{.planar_home = home.planar_home().string(),
                                         .home        = home.root_.string(),
                                         .codex_home  = home.codex_home().string(),
                                         .vendor      = "codex"});

  // Restore permissions before any assertion can throw/REQUIRE-fail, so the
  // scratch_root guard's own destructor can still remove the tree.
  ::chmod(staged.c_str(), 0644);

  // The port's accepted behaviour: the run SUCCEEDS (unlike the oracle,
  // which would die here) and the unreadable staged file classifies exactly
  // like a missing one -- `try_read_file` returns `nullopt` for BOTH.
  REQUIRE(result.has_value());
  REQUIRE(result->projections.size() == 1);
  CHECK(result->projections[0].status != is_::state::fresh);
}

// ---- plan 1104 M2 (task 7219): the nine-root layout. ----

namespace {

/// The scratch install the nine-root tests share: a staged authority under
/// `.planar`, every vendor marker seeded, and the placements the installer
/// would make, in copy or link mode.
struct nine_root_install {
  scratch_root             home;
  bool                     link;
  std::vector<std::string> roles{"coder", "reviewer"};
  std::string              manifest_rows;
  std::size_t              rows = 0;

  explicit nine_root_install(bool link_mode) : link(link_mode) {
    for (auto const* dir : {".claude", ".codex", ".copilot", ".gemini/antigravity-cli", ".config/opencode"}) {
      std::filesystem::create_directories(home.root_ / dir);
    }
    home.write(home.root_ / ".gemini" / "settings.json", "{}\n");
    home.write(skill_src() / "SKILL.md", "---\nname: planar\n---\nskill body\n");
    home.write(skill_src() / "references" / "x.md", "reference\n");
    for (auto const& role : roles) {
      home.write(home.planar_home() / "agents" / ("planar-" + role + ".md"),
                 "---\nname: planar-" + role + "\ndescription: Does " + role + " work\nmodel: sonnet\n---\nbody " + role + "\n");
      home.write(home.planar_home() / "codex-agents" / ("planar-" + role + ".toml"), "name = \"" + role + "\"\n");
    }
    place_skill(".claude/skills", "claude");
    place_skill(".agents/skills", "shared");
    place_skill(".gemini/antigravity-cli/skills", "antigravity");
    place_agents(".claude/agents", "claude", ".md", false);
    place_agents("", "codex", ".toml", false);
    place_agents(".copilot/agents", "copilot", ".agent.md", false);
    place_agents(".gemini/agents", "gemini", ".md", false);
    place_agents(".gemini/antigravity-cli/agents", "antigravity", ".md", false);
    place_agents(".config/opencode/agents", "opencode", ".md", true);
    write_manifest();
  }

  [[nodiscard]] auto skill_src() const -> std::filesystem::path {
    return home.planar_home() / "skills" / "planar";
  }

  void add_row(std::string_view vendor, std::string_view kind, std::string_view name, const std::filesystem::path& staged,
               const std::filesystem::path& installed, std::string_view install_kind) {
    manifest_rows +=
        std::format("{}{{\"vendor\":\"{}\",\"kind\":\"{}\",\"name\":\"{}\",\"staged_path\":\"{}\",\"installed_path\":\"{}\","
                    "\"install_kind\":\"{}\",\"source_digest\":\"\",\"projection_digest\":\"\"}}",
                    rows == 0 ? "" : ",", vendor, kind, name, staged.string(), installed.string(), install_kind);
    ++rows;
  }

  void place_skill(std::string_view root, std::string_view vendor) {
    const auto dst = home.root_ / root / "planar";
    std::filesystem::create_directories(dst.parent_path());
    if (link) {
      std::filesystem::create_directory_symlink(skill_src(), dst);
    } else {
      std::filesystem::copy(skill_src(), dst, std::filesystem::copy_options::recursive);
    }
    add_row(vendor, "skill", "planar", skill_src(), dst, link ? "link" : "copy");
  }

  void place_agents(std::string_view root, std::string_view vendor, std::string_view ext, bool derived) {
    const auto dir = root.empty() ? home.codex_home() / "agents" : home.root_ / root;
    std::filesystem::create_directories(dir);
    for (auto const& role : roles) {
      const auto src = ext == ".toml" ? home.planar_home() / "codex-agents" / ("planar-" + role + ".toml")
                                      : home.planar_home() / "agents" / ("planar-" + role + ".md");
      const auto dst = dir / ("planar-" + role + std::string(ext));
      if (derived) {
        auto bytes = is_::derive_opencode(*slurp(src));
        REQUIRE(bytes.has_value());
        home.write(dst, *bytes);
      } else if (link) {
        std::filesystem::create_symlink(src, dst);
      } else {
        std::filesystem::copy_file(src, dst);
      }
      add_row(vendor, "agent", "planar-" + role, src, dst, (link && !derived) ? "link" : "copy");
    }
  }

  static auto slurp(const std::filesystem::path& path) -> std::optional<std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  void write_manifest() const {
    home.write(home.planar_home() / "install-manifest.json",
               std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"{}\",\"vendors\":[\"claude\",\"codex\","
                           "\"copilot\",\"gemini\",\"antigravity\",\"opencode\"],\"extras\":[],\"projections\":[{}]}}",
                           link ? "link" : "copy", manifest_rows));
  }

  [[nodiscard]] auto options() const -> is_::options {
    return is_::options{
        .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()};
  }
};

/// The root a row's installed path sits in, relative to the scratch `$HOME`.
auto root_label(const nine_root_install& fx, const is_::projection_status& row) -> std::string {
  return std::filesystem::path(row.installed_path).parent_path().lexically_relative(fx.home.root_).generic_string();
}

auto row_roots(const nine_root_install& fx, const is_::status_result& result) -> std::set<std::string> {
  std::set<std::string> out;
  for (auto const& row : result.projections) {
    out.insert(root_label(fx, row));
  }
  return out;
}

} // namespace

TEST_CASE("the nine-root table: nine roots, each read by at least one vendor, no duplicate path",
          "[installed_surface][nine_roots]") {
  scratch_root home;
  const auto   roots = is_::installed_roots(is_::options{
      .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
  REQUIRE(roots.size() == 9);
  std::set<std::string> paths;
  std::size_t           skills = 0;
  for (auto const& root : roots) {
    CAPTURE(root.path);
    CHECK_FALSE(root.vendors.empty());
    for (auto const& vendor : root.vendors) {
      CHECK(std::ranges::find(is_::supported_vendors, vendor) != is_::supported_vendors.end());
    }
    CHECK(root.kind == (root.directory_shape ? "skill" : "agent"));
    skills += root.kind == "skill" ? 1 : 0;
    CHECK(paths.insert(root.path).second);
    CHECK_FALSE(root.present); // an empty scratch home has no vendor markers
  }
  CHECK(skills == 3);
  // The shared root is read by four vendors; the Codex agents root hangs off $CODEX_HOME.
  auto const shared = std::ranges::find(roots, (home.root_ / ".agents" / "skills").string(), &is_::installed_root::path);
  REQUIRE(shared != roots.end());
  CHECK(shared->vendors == std::vector<std::string>{"codex", "copilot", "gemini", "opencode"});
  CHECK(std::ranges::find(roots, (home.codex_home() / "agents").string(), &is_::installed_root::path) != roots.end());
  const auto opencode =
      std::ranges::find(roots, (home.root_ / ".config" / "opencode" / "agents").string(), &is_::installed_root::path);
  REQUIRE(opencode != roots.end());
  CHECK(opencode->derived);
  // None of the retired roots.
  for (auto const& root : roots) {
    for (std::string_view retired : {"/.claude/commands", "/.codex/skills", "/.copilot/skills"}) {
      CHECK_FALSE(root.path.ends_with(retired));
    }
  }
}

TEST_CASE("each vendor's presence marker makes its roots present", "[installed_surface][nine_roots]") {
  auto present_roots = [](const scratch_root& home, bool codex_set) {
    std::set<std::string> out;
    for (auto const& root : is_::installed_roots(is_::options{.planar_home    = home.planar_home().string(),
                                                              .home           = home.root_.string(),
                                                              .codex_home     = home.codex_home().string(),
                                                              .codex_home_set = codex_set})) {
      if (root.present) {
        out.insert(std::filesystem::path(root.path).lexically_relative(home.root_).generic_string());
      }
    }
    return out;
  };
  {
    scratch_root home;
    std::filesystem::create_directories(home.root_ / ".claude");
    CHECK(present_roots(home, false) == std::set<std::string>{".claude/skills", ".claude/agents"});
  }
  {
    scratch_root home; // $CODEX_HOME set: codex is present without ~/.codex/; the shared root follows it
    CHECK(present_roots(home, true) == std::set<std::string>{".agents/skills", ".codex/agents"});
  }
  {
    scratch_root home; // ~/.gemini/ without settings.json is not Gemini CLI
    std::filesystem::create_directories(home.root_ / ".gemini");
    CHECK(present_roots(home, false).empty());
    home.write(home.root_ / ".gemini" / "settings.json", "{}");
    CHECK(present_roots(home, false) == std::set<std::string>{".agents/skills", ".gemini/agents"});
  }
  {
    scratch_root home; // Antigravity is detected on its own directory, not on Gemini's
    std::filesystem::create_directories(home.root_ / ".gemini" / "antigravity-cli");
    CHECK(present_roots(home, false) ==
          std::set<std::string>{".gemini/antigravity-cli/skills", ".gemini/antigravity-cli/agents"});
  }
  {
    scratch_root home;
    std::filesystem::create_directories(home.root_ / ".config" / "opencode");
    CHECK(present_roots(home, false) == std::set<std::string>{".agents/skills", ".config/opencode/agents"});
  }
  {
    scratch_root home;
    std::filesystem::create_directories(home.root_ / ".copilot");
    CHECK(present_roots(home, false) == std::set<std::string>{".agents/skills", ".copilot/agents"});
  }
}

TEST_CASE("a copy install across all six vendors reads fresh in nine roots, then one drifted byte flips one root",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(false);
  auto              result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(result->manifest_status == is_::manifest_state::current);
  CHECK(result->projections.size() == fx.rows);
  CHECK(fx.rows == 3 + 6 * 2);
  CHECK(result->summary.fresh == fx.rows);
  CHECK(result->summary.stale + result->summary.missing + result->summary.unmanaged == 0);
  CHECK_FALSE(result->repair_command.has_value());
  const auto roots = row_roots(fx, *result);
  CHECK(roots == std::set<std::string>{".claude/skills", ".agents/skills", ".gemini/antigravity-cli/skills", ".claude/agents",
                                       ".codex/agents", ".copilot/agents", ".gemini/agents", ".gemini/antigravity-cli/agents",
                                       ".config/opencode/agents"});
  for (auto const& row : result->projections) {
    CHECK(row.install_kind == "copy");
  }

  // One byte in the installed Claude skill copy.
  const auto skill = fx.home.root_ / ".claude" / "skills" / "planar" / "SKILL.md";
  fx.home.write(skill, "---\nname: planar\n---\nskill bodY\n");
  auto drifted = is_::status(fx.options());
  REQUIRE(drifted.has_value());
  CHECK(drifted->summary.stale == 1);
  CHECK(drifted->summary.fresh == fx.rows - 1);
  for (auto const& row : drifted->projections) {
    CHECK((row.status == is_::state::stale) == (row.installed_path == (skill.parent_path()).string()));
  }
  REQUIRE(drifted->repair_command.has_value());
}

TEST_CASE("a skill compares as a directory: an extra, missing or changed file under the copy is drift",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(false);
  const auto        copy        = fx.home.root_ / ".agents" / "skills" / "planar";
  auto              stale_count = [&] {
    auto result = is_::status(fx.options());
    REQUIRE(result.has_value());
    return result->summary.stale;
  };
  REQUIRE(stale_count() == 0);
  fx.home.write(copy / "extra.md", "unrecorded\n");
  CHECK(stale_count() == 1);
  std::filesystem::remove(copy / "extra.md");
  REQUIRE(stale_count() == 0);
  std::filesystem::remove(copy / "references" / "x.md");
  CHECK(stale_count() == 1);
  fx.home.write(copy / "references" / "x.md", "changed\n");
  CHECK(stale_count() == 1);
  fx.home.write(copy / "references" / "x.md", "reference\n");
  CHECK(stale_count() == 0);
  // A copy row whose directory became a symlink to the (byte-identical) staged tree is a shape mismatch.
  std::filesystem::remove_all(copy);
  std::filesystem::create_directory_symlink(fx.skill_src(), copy);
  CHECK(stale_count() == 1);
  std::filesystem::remove(copy);
  auto result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(result->summary.missing == 1);
}

TEST_CASE("a link install reads fresh by construction; the skill is a symlink into the staged tree",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(true);
  const auto        skill = fx.home.root_ / ".claude" / "skills" / "planar";
  REQUIRE(std::filesystem::is_symlink(skill));
  CHECK(std::filesystem::read_symlink(skill) == fx.skill_src());
  auto result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(result->install_mode == "link");
  CHECK(result->summary.fresh == fx.rows);
  CHECK(result->summary.stale + result->summary.missing == 0);
  // OpenCode agents are derived regular files even in link mode.
  for (auto const& row : result->projections) {
    if (row.installed_path.find("/opencode/agents/") != std::string::npos) {
      CHECK(row.install_kind == "copy");
      CHECK_FALSE(std::filesystem::is_symlink(row.installed_path));
    }
  }
  // Re-pointing the link elsewhere is drift.
  std::filesystem::remove(skill);
  std::filesystem::create_directory_symlink(fx.home.root_ / ".agents", skill);
  auto moved = is_::status(fx.options());
  REQUIRE(moved.has_value());
  CHECK(moved->summary.stale == 1);
}

TEST_CASE("an absent vendor produces no row: with only ~/.claude present exactly the two Claude roots appear",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(false);
  for (auto const* dir : {".codex", ".copilot", ".gemini", ".config"}) {
    std::filesystem::remove_all(fx.home.root_ / dir);
  }
  auto result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(row_roots(fx, *result) == std::set<std::string>{".claude/skills", ".claude/agents"});
  CHECK(result->summary.fresh == 3);
  CHECK(result->summary.missing == 0);
  // The installed copies elsewhere are still on disk; they are simply not this host's concern now.
  CHECK(std::filesystem::exists(fx.home.root_ / ".agents" / "skills"));
}

TEST_CASE("the shared skill root reads for any one of its four vendors and the --vendor filter keeps it",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(false);
  for (auto const* dir : {".claude", ".codex", ".copilot", ".gemini", ".config"}) {
    std::filesystem::remove_all(fx.home.root_ / dir);
  }
  std::filesystem::create_directories(fx.home.root_ / ".copilot");
  auto result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(row_roots(fx, *result) == std::set<std::string>{".agents/skills", ".copilot/agents"});
  auto opts   = fx.options();
  opts.vendor = "copilot";
  auto only   = is_::status(opts);
  REQUIRE(only.has_value());
  CHECK(only->projections.size() == 3);
  opts.vendor = "claude"; // claude does not read the shared root
  auto none   = is_::status(opts);
  REQUIRE(none.has_value());
  CHECK(none->projections.empty());
}

TEST_CASE("an OpenCode agent compares against the derivation of the staged file, never the staged bytes",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(false);
  const auto        dst = fx.home.root_ / ".config" / "opencode" / "agents" / "planar-coder.md";
  // The full staged file (with its `model:` line) is not what OpenCode reads.
  std::filesystem::copy_file(fx.home.planar_home() / "agents" / "planar-coder.md", dst,
                             std::filesystem::copy_options::overwrite_existing);
  auto result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(result->summary.stale == 1);
}

TEST_CASE("an unmanaged planar-named entry in a present root is reported; other tools' entries are not",
          "[installed_surface][nine_roots]") {
  nine_root_install fx(false);
  fx.home.write(fx.home.root_ / ".agents" / "skills" / "other-tool" / "SKILL.md", "not ours\n");
  fx.home.write(fx.home.root_ / ".gemini" / "agents" / "planar-old.md", "left behind\n");
  auto result = is_::status(fx.options());
  REQUIRE(result.has_value());
  CHECK(result->summary.unmanaged == 1);
  for (auto const& row : result->projections) {
    if (row.status == is_::state::unmanaged) {
      CHECK(row.name == "planar-old");
      CHECK(row.vendor == "gemini");
    }
  }
}

TEST_CASE("derive_opencode reduces the frontmatter and keeps the body", "[installed_surface][nine_roots]") {
  CHECK(is_::derive_opencode("---\nname: x\ndescription: Does a thing\nmodel: m\n---\nbody\n\nmore\n") ==
        "---\ndescription: Does a thing\nmode: subagent\n---\nbody\n\nmore\n");
  // A description YAML would misread is double-quoted, with `\` and `"` escaped.
  CHECK(is_::derive_opencode("---\ndescription: Use when: it \"fires\"\n---\nb\n") ==
        "---\ndescription: \"Use when: it \\\"fires\\\"\"\nmode: subagent\n---\nb\n");
  CHECK(is_::derive_opencode("---\ndescription: \"already quoted: yes\"\n---\n") ==
        "---\ndescription: \"already quoted: yes\"\nmode: subagent\n---\n");
  // An unterminated final record is still a record and gains its newline, as awk's print does.
  CHECK(is_::derive_opencode("---\ndescription: d\n---\nno newline") == "---\ndescription: d\nmode: subagent\n---\nno newline\n");
  CHECK(is_::derive_opencode("").value_or("x").empty());
  // The refusals: no opening fence, no description, never closed.
  CHECK_FALSE(is_::derive_opencode("name: x\n---\n").has_value());
  CHECK_FALSE(is_::derive_opencode("---\nname: x\n---\nbody\n").has_value());
  CHECK_FALSE(is_::derive_opencode("---\ndescription: d\nbody\n").has_value());
}

TEST_CASE("derive_opencode is byte-equal to install.sh's awk for all fifteen agents", "[installed_surface][nine_roots]") {
  const std::filesystem::path root{PLANAR_TARGET_SOURCE_ROOT};
  scratch_root                scratch;
  // The awk program is lifted out of install.sh itself, so a change to either side is a diff here.
  std::ifstream installer(root / "install.sh");
  REQUIRE(installer.good());
  std::string line, program;
  bool        in_awk = false;
  while (std::getline(installer, line)) {
    if (!in_awk && line.starts_with("opencode_derive() {")) {
      in_awk = true;
      continue;
    }
    if (in_awk) {
      if (line.starts_with("  ' \"$1\"")) {
        break;
      }
      if (line.starts_with("  awk '")) {
        program = line.substr(std::string("  awk '").size()) + "\n";
        continue;
      }
      program += line + "\n";
    }
  }
  REQUIRE_FALSE(program.empty());
  const auto awk_file = scratch.root_ / "derive.awk";
  // `\047` inside the awk source is awk's own escape; install.sh holds it unchanged.
  scratch.write(awk_file, program);

  std::size_t compared = 0;
  for (auto const& entry : std::filesystem::directory_iterator(root / "agents")) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with("planar-") || entry.path().extension() != ".md") {
      continue;
    }
    const auto out = scratch.root_ / ("awk-" + name);
    const auto cmd = std::format("awk -f '{}' '{}' > '{}' 2>/dev/null", awk_file.string(), entry.path().string(), out.string());
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::ifstream     awk_in(out, std::ios::binary);
    std::ifstream     src_in(entry.path(), std::ios::binary);
    const std::string awk_bytes((std::istreambuf_iterator<char>(awk_in)), std::istreambuf_iterator<char>());
    const std::string staged((std::istreambuf_iterator<char>(src_in)), std::istreambuf_iterator<char>());
    CAPTURE(name);
    auto cpp = is_::derive_opencode(staged);
    REQUIRE(cpp.has_value());
    CHECK(*cpp == awk_bytes);
    ++compared;
  }
  CHECK(compared == 15);
}

TEST_CASE("the manifest vocabulary: the six vendors and `shared` are valid, anything else is invalid",
          "[installed_surface][nine_roots]") {
  auto classify = [](std::string_view manifest_vendors, std::string_view row_vendor, std::string_view kind,
                     std::string_view install_kind) {
    scratch_root home;
    home.write(home.planar_home() / "install-manifest.json",
               std::format("{{\"version\":2,\"build_id\":\"t\",\"install_mode\":\"copy\",\"vendors\":[{}],\"projections\":["
                           "{{\"vendor\":\"{}\",\"kind\":\"{}\",\"name\":\"n\",\"staged_path\":\"/s\",\"installed_path\":\"/i\","
                           "\"install_kind\":\"{}\",\"source_digest\":\"\",\"projection_digest\":\"\"}}]}}",
                           manifest_vendors, row_vendor, kind, install_kind));
    auto result = is_::status(is_::options{
        .planar_home = home.planar_home().string(), .home = home.root_.string(), .codex_home = home.codex_home().string()});
    REQUIRE(result.has_value());
    return result->manifest_status;
  };
  for (std::string_view vendor : {"claude", "codex", "copilot", "gemini", "antigravity", "opencode"}) {
    CAPTURE(vendor);
    const auto listed = std::format("\"{}\"", vendor);
    CHECK(classify(listed, vendor, "skill", "copy") == is_::manifest_state::current);
    CHECK(classify(listed, vendor, "agent", "link") == is_::manifest_state::current);
  }
  // `shared` is a root, not a vendor: valid on a row without being listed, never listed itself.
  CHECK(classify("\"codex\"", "shared", "skill", "copy") == is_::manifest_state::current);
  CHECK(classify("\"shared\"", "shared", "skill", "copy") == is_::manifest_state::invalid);
  // A row vendor the manifest never selected, and the retired names.
  CHECK(classify("\"claude\"", "codex", "skill", "copy") == is_::manifest_state::invalid);
  CHECK(classify("\"claude\"", "cursor", "skill", "copy") == is_::manifest_state::invalid);
  CHECK(classify("\"claude\"", "claude", "command", "copy") == is_::manifest_state::invalid);
  CHECK(classify("\"claude\"", "claude", "skill", "symlink") == is_::manifest_state::invalid);
}
