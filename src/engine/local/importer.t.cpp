// @file importer.t.cpp
// @brief Tests for `planar.engine.local.importer` (plan 996, task 6109).
//
// ============================================================================
// HOME SAFETY
// ============================================================================
// `import_sources` takes an explicit `home_dir` and reads no environment. Both
// the external source tree and the destination sandbox live inside one
// `scratch_home` under `temp_directory_path()`, removed on destruction. No
// absolute path outside it appears below.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Fixture: an external directory /tmp/pb/ext holding
//   one.md   valid, `description: Ext one.`
//   bad.md   the single line `garbage` — no frontmatter
//   shad.md  `description:` empty and `shadow: true`
//
//   $Z local import /tmp/pb/ext --json --no-link
//     {"Imported":[
//        {"Name":"one","SourcePath":"/tmp/pb/ext/one.md",
//         "TargetPath":"/tmp/pb/h/.planar/local/skills/one/SKILL.md",
//         "Action":"imported","Reason":""},
//        {"Name":"shad",...,"Action":"imported","Reason":""}],
//      "Skipped":[
//        {"Name":"bad","SourcePath":"/tmp/pb/ext/bad.md","TargetPath":"",
//         "Action":"skipped","Reason":"no-frontmatter"}],
//      "Warnings":[
//        {"Name":"shad","Field":"description","Message":"description is empty; ..."},
//        {"Name":"shad","Field":"shadow","Message":"shadow:true — install will land ..."}]}
//
//   -- note a FLAT source file landed at `skills/one/SKILL.md`: import promotes
//      into directory shape, so nothing imported ever needs `local migrate`.
//   -- note the skipped record's TargetPath is EMPTY, because parsing failed
//      before a destination was computed.
//
//   $Z local import /tmp/pb/ext --no-link      (second run, all colliding)
//     bad           skipped       reason: no-frontmatter
//     one           skipped       reason: name-collision
//     shad          skipped       reason: name-collision
//     warning [shad.description] description is empty; ...
//     warning [shad.shadow] shadow:true — install will land as "shad.md" ...
//     <blank>
//     imported 0 file(s); skipped 3
//
//   -- the warnings survive the collision skip: the operator still wants to
//      know the source they tried to import has an empty description.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.local.manifest;
import planar.engine.local.importer;

namespace mf = planar::engine::local::manifest;
namespace im = planar::engine::local::import_;

namespace {

/// @brief A per-test scratch tree holding BOTH the external source and the
/// destination sandbox, removed on destruction.
struct scratch_home {
  std::filesystem::path root_;

  scratch_home()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_local_imp_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(home() / ".planar" / "local", ec);
    std::filesystem::create_directories(ext(), ec);
  }
  scratch_home(const scratch_home&)            = delete;
  scratch_home& operator=(const scratch_home&) = delete;
  ~scratch_home() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /// @brief Stands in for `$PLANAR_LOCAL_HOME`.
  [[nodiscard]] auto home() const -> std::filesystem::path {
    return root_ / "home";
  }
  /// @brief The external tree being imported FROM.
  [[nodiscard]] auto ext() const -> std::filesystem::path {
    return root_ / "ext";
  }
  [[nodiscard]] auto sandbox() const -> std::filesystem::path {
    return home() / ".planar" / "local";
  }

  auto write(const std::filesystem::path& path, std::string_view content) const -> void {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  [[nodiscard]] auto opts(std::filesystem::path source, mf::kind kind = mf::kind::skill) const -> im::options {
    return im::options{.home_dir = home(), .source_path = std::move(source), .kind = kind};
  }
};

auto names(std::span<const im::record> records) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& rec : records) {
    out.push_back(rec.name);
  }
  return out;
}

constexpr std::string_view k_valid   = "---\ndescription: Ext one.\n---\nx\n";
constexpr std::string_view k_garbage = "garbage\n";

} // namespace

// --- dest_path --------------------------------------------------------------

TEST_CASE("import dest_path PROMOTES a skill into directory shape") {
  // The single most consequential rule here: a flat `one.md` becomes
  // `one/SKILL.md`, so nothing imported ever needs `local migrate`.
  REQUIRE(im::dest_path("/s/skills", "one", mf::kind::skill) == std::filesystem::path{"/s/skills/one/SKILL.md"});
}

TEST_CASE("import dest_path leaves an agent flat") {
  REQUIRE(im::dest_path("/s/agents", "beta", mf::kind::agent) == std::filesystem::path{"/s/agents/beta.md"});
}

// --- the three source shapes ------------------------------------------------

TEST_CASE("import accepts a single FILE") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);

  const auto result = im::import_sources(home.opts(home.ext() / "one.md"));
  REQUIRE(result.has_value());
  REQUIRE(names(result->imported) == std::vector<std::string>{"one"});
  REQUIRE(result->imported[0].action == "imported");
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "one" / "SKILL.md"));
}

TEST_CASE("import accepts a DIRECTORY holding SKILL.md as one skill") {
  // Shape 1, checked FIRST: named for the directory, not treated as a
  // collection that happens to contain a file called SKILL.md.
  const scratch_home home;
  home.write(home.ext() / "mine" / "SKILL.md", k_valid);
  home.write(home.ext() / "mine" / "extra.txt", "auxiliary\n");

  const auto result = im::import_sources(home.opts(home.ext() / "mine"));
  REQUIRE(names(result->imported) == std::vector<std::string>{"mine"});
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "mine" / "SKILL.md"));
  // The whole directory travels, not just the markdown.
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "mine" / "extra.txt"));
}

TEST_CASE("import treats a plain DIRECTORY as a collection, name-sorted") {
  const scratch_home home;
  home.write(home.ext() / "zeta.md", k_valid);
  home.write(home.ext() / "alpha.md", k_valid);

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(names(result->imported) == std::vector<std::string>{"alpha", "zeta"});
}

TEST_CASE("import collects nested skill directories inside a collection") {
  const scratch_home home;
  home.write(home.ext() / "flat.md", k_valid);
  home.write(home.ext() / "nested" / "SKILL.md", k_valid);

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(names(result->imported) == std::vector<std::string>{"flat", "nested"});
}

TEST_CASE("import ignores subdirectories entirely for agents") {
  const scratch_home home;
  home.write(home.ext() / "nested" / "SKILL.md", k_valid);
  home.write(home.ext() / "beta.md", "---\ndescription: b\n---\nx\n");

  const auto result = im::import_sources(home.opts(home.ext(), mf::kind::agent));
  REQUIRE(names(result->imported) == std::vector<std::string>{"beta"});
}

TEST_CASE("import shape 1 does NOT apply under --kind agent") {
  // The SKILL.md probe is guarded on kind, so the same directory falls through
  // to the collection path and yields the entry `SKILL`. Surprising, and
  // exactly what the oracle does.
  const scratch_home home;
  home.write(home.ext() / "mine" / "SKILL.md", "---\ndescription: d\n---\nx\n");

  const auto result = im::import_sources(home.opts(home.ext() / "mine", mf::kind::agent));
  REQUIRE(names(result->imported) == std::vector<std::string>{"SKILL"});
}

TEST_CASE("import skips dotted entries and the link manifest") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);
  home.write(home.ext() / ".hidden.md", k_valid);
  home.write(home.ext() / ".link-manifest.json", "{\"version\":1,\"entries\":[]}");

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(names(result->imported) == std::vector<std::string>{"one"});
}

TEST_CASE("import ignores non-md files in a collection") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);
  home.write(home.ext() / "notes.txt", "not a skill\n");
  REQUIRE(names(im::import_sources(home.opts(home.ext()))->imported) == std::vector<std::string>{"one"});
}

// --- the failure modes that abort the whole pass ----------------------------

TEST_CASE("import FAILS on an empty collection rather than returning nothing") {
  // Contrast `local link` on an empty sandbox, which is a cheerful exit 0. The
  // two leaves genuinely disagree and both spellings are pinned.
  const scratch_home home;
  REQUIRE(im::import_sources(home.opts(home.ext())).error() == im::import_error::not_found);
}

TEST_CASE("import FAILS on a non-md file") {
  const scratch_home home;
  home.write(home.ext() / "notes.txt", "not a skill\n");
  REQUIRE(im::import_sources(home.opts(home.ext() / "notes.txt")).error() == im::import_error::invalid_input);
}

TEST_CASE("import FAILS on an absent path") {
  const scratch_home home;
  // Not a directory and not `.md`, so it takes the invalid_input arm rather
  // than reporting "nothing found".
  REQUIRE(im::import_sources(home.opts(home.ext() / "nope")).error() == im::import_error::invalid_input);
}

TEST_CASE("import refuses empty arguments") {
  const scratch_home home;
  REQUIRE(im::import_sources({.home_dir = "", .source_path = home.ext()}).error() == im::import_error::invalid_input);
  REQUIRE(im::import_sources({.home_dir = home.home(), .source_path = ""}).error() == im::import_error::invalid_input);
}

// --- per-entry skips --------------------------------------------------------

TEST_CASE("import skips an unparseable entry with a reason and an EMPTY target") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);
  home.write(home.ext() / "bad.md", k_garbage);

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(names(result->imported) == std::vector<std::string>{"one"});
  REQUIRE(result->skipped.size() == 1);
  REQUIRE(result->skipped[0].name == "bad");
  REQUIRE(result->skipped[0].reason == "no-frontmatter");
  // Empty because parsing failed BEFORE a destination was computed.
  REQUIRE(result->skipped[0].target_path.empty());
}

TEST_CASE("import skips a collision and does not overwrite") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);
  home.write(home.sandbox() / "skills" / "one" / "SKILL.md", "---\ndescription: original\n---\nkeep me\n");

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(result->imported.empty());
  REQUIRE(result->skipped.size() == 1);
  REQUIRE(result->skipped[0].reason == "name-collision");
  // And the destination is untouched.
  std::ifstream      in(home.sandbox() / "skills" / "one" / "SKILL.md", std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  REQUIRE(buf.str().find("keep me") != std::string::npos);
}

TEST_CASE("import --force overwrites and says so") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);
  home.write(home.sandbox() / "skills" / "one" / "SKILL.md", "---\ndescription: original\n---\nold\n");

  auto opts         = home.opts(home.ext());
  opts.force        = true;
  const auto result = im::import_sources(opts);
  REQUIRE(result->imported.size() == 1);
  // `overwrote`, NOT `imported` — the operator can see from the output which
  // entries replaced something.
  REQUIRE(result->imported[0].action == "overwrote");
}

TEST_CASE("import --force on a directory skill DELETES the old tree") {
  // A real data-loss path, asserted so it cannot be "improved" into a merge
  // without someone noticing: files in the old skill and absent from the new
  // one are GONE.
  const scratch_home home;
  home.write(home.ext() / "mine" / "SKILL.md", k_valid);
  home.write(home.sandbox() / "skills" / "mine" / "SKILL.md", "---\ndescription: old\n---\n");
  home.write(home.sandbox() / "skills" / "mine" / "stale.txt", "should not survive\n");

  auto opts  = home.opts(home.ext() / "mine");
  opts.force = true;
  REQUIRE(im::import_sources(opts)->imported.size() == 1);
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "mine" / "SKILL.md"));
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / "mine" / "stale.txt"));
}

TEST_CASE("import --dry-run reports would-import and writes nothing") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);

  auto opts         = home.opts(home.ext());
  opts.dry_run      = true;
  const auto result = im::import_sources(opts);
  REQUIRE(result->imported.size() == 1);
  REQUIRE(result->imported[0].action == "would-import");
  // The target path is still REPORTED, so the operator can see where it would
  // go; it just does not exist.
  REQUIRE(result->imported[0].target_path == (home.sandbox() / "skills" / "one" / "SKILL.md").string());
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / "one"));
}

TEST_CASE("import --dry-run does not even create the destination directory") {
  const scratch_home home;
  home.write(home.ext() / "beta.md", "---\ndescription: b\n---\nx\n");
  auto opts    = home.opts(home.ext(), mf::kind::agent);
  opts.dry_run = true;
  im::import_sources(opts);
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "agents"));
}

// --- warnings ---------------------------------------------------------------

TEST_CASE("import collects lint warnings, name-then-emission ordered") {
  const scratch_home home;
  home.write(home.ext() / "shad.md", "---\ndescription: \nshadow: true\n---\ny\n");

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(result->warnings.size() == 2);
  REQUIRE(result->warnings[0].name == "shad");
  REQUIRE(result->warnings[0].field == "description");
  REQUIRE(result->warnings[1].field == "shadow");
  REQUIRE(result->warnings[1].message.starts_with("shadow:true — install will land as \"shad.md\""));
}

TEST_CASE("import keeps warnings for an entry it SKIPPED for a collision") {
  // The operator still wants to know the source they tried to import has an
  // empty description, even though nothing was copied. Oracle-captured.
  const scratch_home home;
  home.write(home.ext() / "shad.md", "---\ndescription: \nshadow: true\n---\ny\n");
  home.write(home.sandbox() / "skills" / "shad" / "SKILL.md", "---\ndescription: existing\n---\n");

  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(result->imported.empty());
  REQUIRE(result->skipped.size() == 1);
  REQUIRE(result->warnings.size() == 2);
}

TEST_CASE("import produces NO warnings for an entry that failed to parse") {
  // There is no frontmatter to lint, so a parse failure yields a skip and
  // nothing else.
  const scratch_home home;
  home.write(home.ext() / "bad.md", k_garbage);
  const auto result = im::import_sources(home.opts(home.ext()));
  REQUIRE(result->skipped.size() == 1);
  REQUIRE(result->warnings.empty());
}

// --- copy fidelity ----------------------------------------------------------

TEST_CASE("import copies file bytes verbatim") {
  const scratch_home         home;
  constexpr std::string_view body = "---\ndescription: d\n---\nline one\nline two\n";
  home.write(home.ext() / "one.md", body);
  im::import_sources(home.opts(home.ext()));

  std::ifstream      in(home.sandbox() / "skills" / "one" / "SKILL.md", std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  REQUIRE(buf.str() == body);
}

TEST_CASE("import leaves no .planar-tmp file behind") {
  const scratch_home home;
  home.write(home.ext() / "one.md", k_valid);
  im::import_sources(home.opts(home.ext()));
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / "one" / "SKILL.md.planar-tmp"));
}

TEST_CASE("import copies a nested directory recursively") {
  const scratch_home home;
  home.write(home.ext() / "mine" / "SKILL.md", k_valid);
  home.write(home.ext() / "mine" / "sub" / "deep.txt", "deep\n");

  im::import_sources(home.opts(home.ext() / "mine"));
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "mine" / "sub" / "deep.txt"));
}
