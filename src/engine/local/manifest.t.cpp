// @file manifest.t.cpp
// @brief Tests for `planar.engine.local.manifest` (plan 996, task 6109).
//
// ============================================================================
// HOME SAFETY — read this before adding a test to this file.
// ============================================================================
// `planar local *` writes symlinks into the operator's REAL vendor directories.
// A test that resolves the wrong root does not fail; it edits the developer's
// machine. Two rules keep that impossible here, and both are structural rather
// than conventional:
//
//   1. Nothing in the module under test calls `std::getenv`.
//      `resolve_home_and_root` takes an env-lookup CALLABLE, and this file only
//      ever hands it `env_of({...})` over a scratch path. There is no code path
//      by which the real `$HOME` could be read.
//   2. Every filesystem-touching test operates inside a `scratch_home`, a
//      uniquely-named directory under `temp_directory_path()` removed on
//      destruction. No absolute path outside it appears anywhere below.
//
// The resolve_home_and_root tests deliberately assert the PLANAR_LOCAL_HOME >
// HOME > failure precedence, and that PLANAR_HOME is IGNORED — that last one is
// the trap this bucket exists around, and a regression there is exactly what
// would start writing into a real home.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Fixture: PLANAR_LOCAL_HOME=PLANAR_HOME_PARENT=HOME=/tmp/pb/h, a scratch
// PLANAR_DB and PLANAR_CONFIG_PATH, sandbox at /tmp/pb/h/.planar/local.
// Captured with `od -c` on separated stdout/stderr files, never through a pipe.
//
// Walk-error identifiers, from `local link --dry-run` over a sandbox seeded
// with one broken skill of each shape:
//
//   warning: .../skills/nofm/SKILL.md: NoFrontmatter
//   warning: .../skills/nodashes/SKILL.md: MalformedFrontmatter
//   warning: .../skills/badvendor/SKILL.md: InvalidVendor
//   warning: .../skills/kindmm/SKILL.md: KindMismatch
//   warning: .../skills/emptydir/SKILL.md: skill directory missing SKILL.md
//   warning: .../skills/legacyflat.md: legacy flat skill file; run
//            `planar local migrate` to convert to legacyflat/SKILL.md
//
// These are Zig `@errorName` strings reaching user-visible output, so they are
// frozen identifiers rather than internal names.
//
// On-disk `.link-manifest.json` (skills), captured verbatim:
//
//   {\n  "version": 1,\n  "entries": [\n    {\n      "name": "alpha",\n
//     "source_path": ".../skills/alpha/SKILL.md",\n      "links": [\n
//       {\n          "vendor": "codex",\n ... "linked_at": "2026-08-23T04:12:38Z"\n
//         }\n      ]\n    }\n  ]\n}\n
//
// and the agents manifest with an emptied entry, showing `"links": []` inline:
//
//   {\n  "version": 1,\n  "entries": [\n    {\n      "name": "beta",\n
//     "source_path": ".../agents/beta.md",\n      "links": []\n    }\n  ]\n}\n
//
// `local migrate --json` on a sandbox holding `skills/oldskill.md`:
//   {"Migrated":[{"Name":"oldskill","OldPath":".../skills/oldskill.md",
//     "NewPath":".../skills/oldskill/SKILL.md","Reason":""}],"Skipped":[]}
// — byte-identical with and without `--dry-run`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.local.manifest;

namespace mf = planar::engine::local::manifest;

namespace {

/// @brief A per-test scratch tree, removed on destruction.
///
/// `root_` plays the part of `$PLANAR_LOCAL_HOME`, so the sandbox sits at
/// `root_/.planar/local` exactly as it would on a real machine. Nothing outside
/// `root_` is ever written.
struct scratch_home {
  std::filesystem::path root_;

  scratch_home()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_local_mf_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(sandbox() / "skills", ec);
    std::filesystem::create_directories(sandbox() / "agents", ec);
  }
  scratch_home(const scratch_home&)            = delete;
  scratch_home& operator=(const scratch_home&) = delete;
  ~scratch_home() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] auto sandbox() const -> std::filesystem::path {
    return root_ / ".planar" / "local";
  }

  auto write(const std::filesystem::path& path, std::string_view content) const -> void {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  auto write_skill(std::string_view name, std::string_view content) const -> void {
    write(sandbox() / "skills" / std::string{name} / "SKILL.md", content);
  }
  auto write_agent(std::string_view name, std::string_view content) const -> void {
    write(sandbox() / "agents" / std::format("{}.md", name), content);
  }
};

/// @brief A synthetic environment — the ONLY environment this file ever hands
/// to resolve_home_and_root(). `std::getenv` is never called.
auto env_of(std::map<std::string, std::string, std::less<>> vars) -> std::function<std::optional<std::string>(std::string_view)> {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(name);
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

/// @brief Whether any walk error mentions `needle`.
///
/// Membership, never position: walk_errors is emitted in DIRECTORY-ITERATION
/// order on both sides of the port, which is unspecified. The SET is the
/// contract; the order is not.
auto has_error(const mf::walk_result& result, std::string_view path_fragment, std::string_view message) -> bool {
  return std::ranges::any_of(result.walk_errors, [&](const mf::walk_error& e) {
    return e.path.find(path_fragment) != std::string::npos && e.message == message;
  });
}

} // namespace

// --- HOME safety: the resolution precedence itself --------------------------

TEST_CASE("manifest resolve_home_and_root prefers PLANAR_LOCAL_HOME") {
  const auto resolved = mf::resolve_home_and_root(env_of({{"PLANAR_LOCAL_HOME", "/scratch/local"}, {"HOME", "/scratch/home"}}));
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->home_dir == std::filesystem::path{"/scratch/local"});
  REQUIRE(resolved->sandbox_root == std::filesystem::path{"/scratch/local/.planar/local"});
}

TEST_CASE("manifest resolve_home_and_root falls back to HOME") {
  const auto resolved = mf::resolve_home_and_root(env_of({{"HOME", "/scratch/home"}}));
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->home_dir == std::filesystem::path{"/scratch/home"});
}

TEST_CASE("manifest resolve_home_and_root IGNORES PLANAR_HOME") {
  // THE trap this bucket exists around. Every other Planar surface honours
  // PLANAR_HOME; this one does not. If this test ever starts passing because
  // PLANAR_HOME was consulted, a developer redirecting only PLANAR_HOME would
  // be silently writing into their real ~/.claude/commands.
  const auto resolved = mf::resolve_home_and_root(env_of({{"PLANAR_HOME", "/scratch/planar-home"}, {"HOME", "/scratch/home"}}));
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->home_dir == std::filesystem::path{"/scratch/home"});
}

TEST_CASE("manifest resolve_home_and_root fails when neither variable is set") {
  REQUIRE_FALSE(mf::resolve_home_and_root(env_of({{"PLANAR_HOME", "/scratch/x"}})).has_value());
  REQUIRE_FALSE(mf::resolve_home_and_root(env_of({})).has_value());
}

TEST_CASE("manifest resolve_home_and_root treats an EMPTY value as unset") {
  // Zig's `getPosix` returns the empty string for `PLANAR_LOCAL_HOME=`, and an
  // empty home would resolve the sandbox to the RELATIVE path `.planar/local`
  // — i.e. the current working directory. Falling through to HOME is the safe
  // reading and the one implemented.
  const auto resolved = mf::resolve_home_and_root(env_of({{"PLANAR_LOCAL_HOME", ""}, {"HOME", "/scratch/home"}}));
  REQUIRE(resolved.has_value());
  REQUIRE(resolved->home_dir == std::filesystem::path{"/scratch/home"});
}

// --- frontmatter grammar ----------------------------------------------------

TEST_CASE("manifest split_frontmatter reads the recognised keys") {
  const auto parsed = mf::split_frontmatter("---\n"
                                            "description: Alpha skill.\n"
                                            "argument-hint: <name>\n"
                                            "tier: medium\n"
                                            "model: claude-opus-5\n"
                                            "kind: skill\n"
                                            "shadow: true\n"
                                            "---\n"
                                            "body text\n");
  REQUIRE(parsed.has_value());
  const auto& [fm, body] = *parsed;
  REQUIRE(fm.description == "Alpha skill.");
  REQUIRE(fm.argument_hint == "<name>");
  REQUIRE(fm.tier == "medium");
  REQUIRE(fm.model == "claude-opus-5");
  REQUIRE(fm.kind == "skill");
  REQUIRE(fm.shadow);
  REQUIRE(body == "body text\n");
}

TEST_CASE("manifest split_frontmatter requires the fence at offset 0") {
  REQUIRE(mf::split_frontmatter("no frontmatter here\n").error() == mf::parse_error::no_frontmatter);
  // A LEADING BLANK LINE is fatal here, unlike the workflow `@meta` block which
  // tolerates one. The two formats genuinely differ and neither should be
  // "fixed" to match the other.
  REQUIRE(mf::split_frontmatter("\n---\nkind: skill\n---\n").error() == mf::parse_error::no_frontmatter);
}

TEST_CASE("manifest split_frontmatter rejects an unterminated block") {
  REQUIRE(mf::split_frontmatter("---\ndescription: unterminated\n").error() == mf::parse_error::malformed_frontmatter);
}

TEST_CASE("manifest split_frontmatter accepts a block closed at EOF") {
  // `\n---` with no trailing newline. Zig computes the close offset as
  // `rest.len - 4` and then still skips `end + 5`, one past the end, which is
  // why the body is EMPTY rather than out of range.
  const auto parsed = mf::split_frontmatter("---\nkind: skill\n---");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->first.kind == "skill");
  REQUIRE(parsed->second.empty());
}

TEST_CASE("manifest frontmatter takes the LAST duplicate key") {
  const auto parsed = mf::split_frontmatter("---\ndescription: first\ndescription: second\n---\n");
  REQUIRE(parsed->first.description == "second");
}

TEST_CASE("manifest frontmatter strips one matching quote pair") {
  REQUIRE(mf::split_frontmatter("---\ndescription: \"quoted\"\n---\n")->first.description == "quoted");
  REQUIRE(mf::split_frontmatter("---\ndescription: 'quoted'\n---\n")->first.description == "quoted");
  // Mismatched quotes are left ALONE, not half-stripped.
  REQUIRE(mf::split_frontmatter("---\ndescription: 'quoted\"\n---\n")->first.description == "'quoted\"");
}

TEST_CASE("manifest frontmatter shadow is the exact string true") {
  REQUIRE(mf::split_frontmatter("---\nshadow: true\n---\n")->first.shadow);
  // Every near-miss is FALSE. shadow:true removes the `local-` prefix and can
  // replace a canonical install, so a typo must fail closed.
  REQUIRE_FALSE(mf::split_frontmatter("---\nshadow: True\n---\n")->first.shadow);
  REQUIRE_FALSE(mf::split_frontmatter("---\nshadow: yes\n---\n")->first.shadow);
  REQUIRE_FALSE(mf::split_frontmatter("---\nshadow: 1\n---\n")->first.shadow);
}

TEST_CASE("manifest frontmatter reads all three vendors forms") {
  const auto flow = mf::split_frontmatter("---\nvendors: [claude, codex]\n---\n");
  REQUIRE(flow->first.vendors == std::vector<std::string>{"claude", "codex"});

  const auto scalar = mf::split_frontmatter("---\nvendors: claude\n---\n");
  REQUIRE(scalar->first.vendors == std::vector<std::string>{"claude"});

  const auto block = mf::split_frontmatter("---\nvendors:\n  - claude\n  - codex\n---\n");
  REQUIRE(block->first.vendors == std::vector<std::string>{"claude", "codex"});
}

TEST_CASE("manifest frontmatter strips inline comments from vendors ONLY") {
  const auto block = mf::split_frontmatter("---\ndescription: x # y\nvendors:\n  - claude # a comment\n---\n");
  REQUIRE(block->first.vendors == std::vector<std::string>{"claude"});
  // The description KEEPS its `# y`. This looks like a bug and is not one:
  // strip_inline_comment is applied to vendor items and nowhere else.
  REQUIRE(block->first.description == "x # y");
}

TEST_CASE("manifest frontmatter vendors block survives a blank line") {
  // The blank-line `continue` happens BEFORE `mode_vendors` is cleared, so a
  // blank line inside a block list does not end it.
  const auto parsed = mf::split_frontmatter("---\nvendors:\n  - claude\n\n  - codex\n---\n");
  REQUIRE(parsed->first.vendors == std::vector<std::string>{"claude", "codex"});
}

TEST_CASE("manifest frontmatter ends a vendors block at the next key") {
  const auto parsed = mf::split_frontmatter("---\nvendors:\n  - claude\ntier: medium\n---\n");
  REQUIRE(parsed->first.vendors == std::vector<std::string>{"claude"});
  REQUIRE(parsed->first.tier == "medium");
}

TEST_CASE("manifest frontmatter ignores unknown keys and colonless lines") {
  const auto parsed = mf::split_frontmatter("---\nmystery: value\njust a line\nkind: skill\n---\n");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->first.kind == "skill");
}

TEST_CASE("manifest frontmatter reads kind from the planar map and still accepts a top-level kind") {
  const auto namespaced = mf::split_frontmatter("---\nname: x\ndescription: d\nplanar:\n  kind: agent\n  slug: x\n---\n");
  REQUIRE(namespaced.has_value());
  REQUIRE(namespaced->first.kind == "agent");
  REQUIRE(namespaced->first.description == "d");

  // Deprecated top-level form: operator-local files are not renamed by Planar.
  const auto flat = mf::split_frontmatter("---\ndescription: d\nkind: agent\n---\n");
  REQUIRE(flat.has_value());
  REQUIRE(flat->first.kind == "agent");

  // A column-0 key closes the planar map: the planar block here has no kind, so kind stays empty.
  const auto closed = mf::split_frontmatter("---\nplanar:\n  slug: x\ntier: medium\n---\n");
  REQUIRE(closed.has_value());
  REQUIRE(closed->first.tier == "medium");
  REQUIRE(closed->first.kind.empty());
}

TEST_CASE("manifest frontmatter tolerates CR inside the block but NOT in the fences") {
  // I wrote this test the wrong way round first — assuming that because each
  // line is trimmed of " \t\r", a wholly-CRLF file would parse. It does not,
  // and the oracle settled it. Two fixtures were placed in a real sandbox and
  // `local link --dry-run --json` was run over both:
  //
  //   skills/crlf/SKILL.md   every line CRLF        -> DROPPED (walk error)
  //   skills/mixed/SKILL.md  LF fences, CR keys     -> parsed, description "mixed"
  //
  // The fences are matched as the literal bytes `---\n` and `\n---\n` BEFORE
  // any line is trimmed, so a `\r` in a fence is fatal while a `\r` in a key
  // line is not. A port that normalised newlines up front would accept the
  // first fixture and silently install a skill the oracle refuses.
  const auto crlf = mf::split_frontmatter("---\r\ndescription: crlf\r\nkind: skill\r\n---\r\nbody\r\n");
  REQUIRE_FALSE(crlf.has_value());
  REQUIRE(crlf.error() == mf::parse_error::no_frontmatter);

  const auto mixed = mf::split_frontmatter("---\ndescription: mixed\r\nkind: skill\r\n---\nbody\n");
  REQUIRE(mixed.has_value());
  REQUIRE(mixed->first.description == "mixed");
  REQUIRE(mixed->first.kind == "skill");
  REQUIRE(mixed->second == "body\n");
}

// --- resolved_vendors and lint ----------------------------------------------

TEST_CASE("manifest resolved_vendors expands an empty list to all three") {
  // EMPTY means "all three", not "none". An author who omitted the key never
  // meant an uninstallable skill.
  REQUIRE(mf::resolved_vendors({}) == std::vector<std::string>{"claude", "codex", "copilot"});
}

TEST_CASE("manifest resolved_vendors sorts what the author wrote") {
  mf::frontmatter fm;
  fm.vendors = {"copilot", "claude"};
  REQUIRE(mf::resolved_vendors(fm) == std::vector<std::string>{"claude", "copilot"});
}

TEST_CASE("manifest lint warns about an empty description") {
  const auto issues = mf::lint({}, "alpha");
  REQUIRE(issues.size() == 1);
  REQUIRE(issues[0].field == "description");
  REQUIRE(issues[0].message == "description is empty; vendors surface this as the skill summary");
}

TEST_CASE("manifest lint warns about shadow, naming the exact filename") {
  // Oracle capture from `local import`'s warnings array; the em dash is U+2014.
  mf::frontmatter fm;
  fm.description    = "present";
  fm.shadow         = true;
  const auto issues = mf::lint(fm, "shad");
  REQUIRE(issues.size() == 1);
  REQUIRE(issues[0].field == "shadow");
  REQUIRE(issues[0].message == "shadow:true — install will land as \"shad.md\" (no local- prefix) and may replace a canonical "
                               "install of the same name");
}

TEST_CASE("manifest lint reports description before shadow") {
  // The ORDER is observable: `local import` renders warnings in this sequence
  // and the oracle capture shows description first.
  mf::frontmatter fm;
  fm.shadow         = true;
  const auto issues = mf::lint(fm, "shad");
  REQUIRE(issues.size() == 2);
  REQUIRE(issues[0].field == "description");
  REQUIRE(issues[1].field == "shadow");
}

// --- parse_file -------------------------------------------------------------

TEST_CASE("manifest parse_file names a skill for its DIRECTORY") {
  const scratch_home home;
  home.write_skill("alpha", "---\ndescription: Alpha skill.\nkind: skill\n---\nalpha body\n");
  const auto parsed = mf::parse_file(home.sandbox() / "skills" / "alpha" / "SKILL.md", mf::kind::skill);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->name == "alpha");
  REQUIRE(parsed->body == "alpha body\n");
}

TEST_CASE("manifest parse_file names an agent for its STEM") {
  const scratch_home home;
  home.write_agent("beta", "---\ndescription: Beta agent.\nkind: agent\n---\nbeta body\n");
  const auto parsed = mf::parse_file(home.sandbox() / "agents" / "beta.md", mf::kind::agent);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->name == "beta");
}

TEST_CASE("manifest parse_file rejects a kind that disagrees with the directory") {
  const scratch_home home;
  home.write_skill("kindmm", "---\ndescription: km\nkind: agent\n---\nb\n");
  REQUIRE(mf::parse_file(home.sandbox() / "skills" / "kindmm" / "SKILL.md", mf::kind::skill).error() ==
          mf::parse_error::kind_mismatch);
}

TEST_CASE("manifest parse_file accepts an ABSENT kind field") {
  // Only a non-empty disagreeing value is rejected. Omitting `kind:` entirely
  // is normal and must stay legal.
  const scratch_home home;
  home.write_skill("nokind", "---\ndescription: nk\n---\nb\n");
  REQUIRE(mf::parse_file(home.sandbox() / "skills" / "nokind" / "SKILL.md", mf::kind::skill).has_value());
}

TEST_CASE("manifest parse_file rejects an unknown vendor") {
  const scratch_home home;
  home.write_skill("badvendor", "---\ndescription: bv\nvendors: [claude, bogus]\n---\nb\n");
  REQUIRE(mf::parse_file(home.sandbox() / "skills" / "badvendor" / "SKILL.md", mf::kind::skill).error() ==
          mf::parse_error::invalid_vendor);
}

TEST_CASE("manifest parse_file reports a missing file distinctly") {
  const scratch_home home;
  REQUIRE(mf::parse_file(home.sandbox() / "skills" / "absent" / "SKILL.md", mf::kind::skill).error() ==
          mf::parse_error::file_not_found);
}

// --- the frozen error identifiers -------------------------------------------

TEST_CASE("manifest parse_error_name matches the Zig @errorName strings") {
  // These reach user-visible `warning:` lines, so they are a contract, not
  // internal names. Oracle-captured; see this file's header.
  REQUIRE(mf::parse_error_name(mf::parse_error::no_frontmatter) == "NoFrontmatter");
  REQUIRE(mf::parse_error_name(mf::parse_error::malformed_frontmatter) == "MalformedFrontmatter");
  REQUIRE(mf::parse_error_name(mf::parse_error::invalid_vendor) == "InvalidVendor");
  REQUIRE(mf::parse_error_name(mf::parse_error::kind_mismatch) == "KindMismatch");
  REQUIRE(mf::parse_error_name(mf::parse_error::invalid_input) == "InvalidInput");
  REQUIRE(mf::parse_error_name(mf::parse_error::file_not_found) == "FileNotFound");
}

TEST_CASE("manifest parse_error_reason is a DIFFERENT vocabulary from the names") {
  // `local import` says `no-frontmatter` where `local link` says
  // `NoFrontmatter`. Three failures collapse onto one reason code; that
  // collapse is the oracle's and is asserted so a future refactor cannot
  // quietly split them.
  REQUIRE(mf::parse_error_reason(mf::parse_error::no_frontmatter) == "no-frontmatter");
  REQUIRE(mf::parse_error_reason(mf::parse_error::kind_mismatch) == "kind-mismatch");
  REQUIRE(mf::parse_error_reason(mf::parse_error::invalid_vendor) == "invalid-vendor");
  REQUIRE(mf::parse_error_reason(mf::parse_error::malformed_frontmatter) == "invalid-frontmatter");
  REQUIRE(mf::parse_error_reason(mf::parse_error::invalid_input) == "invalid-frontmatter");
  REQUIRE(mf::parse_error_reason(mf::parse_error::file_not_found) == "invalid-frontmatter");
}

// --- walk_sandbox -----------------------------------------------------------

TEST_CASE("manifest walk_sandbox returns nothing for an absent root") {
  const scratch_home home;
  const auto         result = mf::walk_sandbox(home.root_ / "does-not-exist");
  REQUIRE(result.files.empty());
  REQUIRE(result.walk_errors.empty());
}

TEST_CASE("manifest walk_sandbox sorts skills before agents, then by name") {
  const scratch_home home;
  home.write_skill("zeta", "---\ndescription: z\n---\nb\n");
  home.write_skill("alpha", "---\ndescription: a\n---\nb\n");
  home.write_agent("beta", "---\ndescription: b\n---\nb\n");

  const auto result = mf::walk_sandbox(home.sandbox());
  REQUIRE(result.files.size() == 3);
  REQUIRE(result.files[0].name == "alpha");
  REQUIRE(result.files[0].kind == mf::kind::skill);
  REQUIRE(result.files[1].name == "zeta");
  REQUIRE(result.files[1].kind == mf::kind::skill);
  // The agent sorts LAST despite `beta` < `zeta`, because kind is the primary
  // key. Reversing the enumerator order would silently invert this.
  REQUIRE(result.files[2].name == "beta");
  REQUIRE(result.files[2].kind == mf::kind::agent);
}

TEST_CASE("manifest walk_sandbox reports each broken shape with its own identifier") {
  const scratch_home home;
  home.write_skill("nofm", "no frontmatter here\n");
  home.write_skill("nodashes", "---\ndescription: unterminated\n");
  home.write_skill("badvendor", "---\ndescription: bv\nvendors: [claude, bogus]\n---\nb\n");
  home.write_skill("kindmm", "---\ndescription: km\nkind: agent\n---\nb\n");
  std::error_code ec;
  std::filesystem::create_directories(home.sandbox() / "skills" / "emptydir", ec);
  home.write(home.sandbox() / "skills" / "legacyflat.md", "---\nkind: skill\n---\nlegacy\n");

  const auto result = mf::walk_sandbox(home.sandbox());
  REQUIRE(result.files.empty());
  REQUIRE(result.walk_errors.size() == 6);
  REQUIRE(has_error(result, "nofm/SKILL.md", "NoFrontmatter"));
  REQUIRE(has_error(result, "nodashes/SKILL.md", "MalformedFrontmatter"));
  REQUIRE(has_error(result, "badvendor/SKILL.md", "InvalidVendor"));
  REQUIRE(has_error(result, "kindmm/SKILL.md", "KindMismatch"));
  REQUIRE(has_error(result, "emptydir/SKILL.md", "skill directory missing SKILL.md"));
  REQUIRE(
      has_error(result, "legacyflat.md", "legacy flat skill file; run `planar local migrate` to convert to legacyflat/SKILL.md"));
}

TEST_CASE("manifest walk_sandbox skips dotted entries, including the manifest") {
  const scratch_home home;
  home.write(home.sandbox() / "skills" / ".link-manifest.json", "{\"version\":1,\"entries\":[]}");
  home.write(home.sandbox() / "skills" / ".hidden.md", "---\nkind: skill\n---\n");
  const auto result = mf::walk_sandbox(home.sandbox());
  REQUIRE(result.files.empty());
  // And NO walk error either — a dotted file is invisible, not broken. Without
  // the dot-skip the manifest itself would be reported as a legacy flat skill
  // on every single run.
  REQUIRE(result.walk_errors.empty());
}

TEST_CASE("manifest walk_sandbox ignores non-md files under skills") {
  const scratch_home home;
  home.write(home.sandbox() / "skills" / "notes.txt", "not a skill");
  const auto result = mf::walk_sandbox(home.sandbox());
  REQUIRE(result.files.empty());
  REQUIRE(result.walk_errors.empty());
}

TEST_CASE("manifest walk_sandbox ignores subdirectories under agents") {
  const scratch_home home;
  std::error_code    ec;
  std::filesystem::create_directories(home.sandbox() / "agents" / "adir", ec);
  const auto result = mf::walk_sandbox(home.sandbox());
  REQUIRE(result.files.empty());
  REQUIRE(result.walk_errors.empty());
}

// --- migrate ----------------------------------------------------------------

TEST_CASE("manifest migrate promotes a flat skill into directory shape") {
  const scratch_home home;
  home.write(home.sandbox() / "skills" / "oldskill.md", "---\nkind: skill\n---\nlegacy body\n");

  const auto result = mf::migrate(home.sandbox(), false);
  REQUIRE(result.migrated.size() == 1);
  REQUIRE(result.skipped.empty());
  REQUIRE(result.migrated[0].name == "oldskill");
  REQUIRE(result.migrated[0].reason.empty());
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "oldskill" / "SKILL.md"));
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / "oldskill.md"));
}

TEST_CASE("manifest migrate --dry-run reports without moving anything") {
  const scratch_home home;
  home.write(home.sandbox() / "skills" / "oldskill.md", "---\nkind: skill\n---\nlegacy\n");

  const auto result = mf::migrate(home.sandbox(), true);
  REQUIRE(result.migrated.size() == 1);
  // The source is STILL THERE. This is the assertion that distinguishes a real
  // dry run from one that reports correctly and moves the file anyway.
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "oldskill.md"));
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / "oldskill" / "SKILL.md"));
}

TEST_CASE("manifest migrate reports already-migrated and collision DIFFERENTLY") {
  const scratch_home home;
  home.write(home.sandbox() / "skills" / "done.md", "---\nkind: skill\n---\n");
  home.write(home.sandbox() / "skills" / "done" / "SKILL.md", "---\nkind: skill\n---\n");
  home.write(home.sandbox() / "skills" / "clash.md", "---\nkind: skill\n---\n");
  std::error_code ec;
  std::filesystem::create_directories(home.sandbox() / "skills" / "clash", ec);

  const auto result = mf::migrate(home.sandbox(), false);
  REQUIRE(result.migrated.empty());
  REQUIRE(result.skipped.size() == 2);
  // Sorted by name, so `clash` precedes `done`.
  REQUIRE(result.skipped[0].name == "clash");
  REQUIRE(result.skipped[0].reason.starts_with("collision: "));
  REQUIRE(result.skipped[0].reason.ends_with(" exists but is not a matching skill dir"));
  REQUIRE(result.skipped[1].name == "done");
  REQUIRE(result.skipped[1].reason.starts_with("already migrated: "));
  REQUIRE(result.skipped[1].reason.ends_with(" exists"));
  // Both sources survive: a skip must never delete.
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "clash.md"));
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "done.md"));
}

TEST_CASE("manifest migrate on an absent skills directory is empty, not an error") {
  const scratch_home home;
  std::error_code    ec;
  std::filesystem::remove_all(home.sandbox() / "skills", ec);
  const auto result = mf::migrate(home.sandbox(), false);
  REQUIRE(result.migrated.empty());
  REQUIRE(result.skipped.empty());
}

TEST_CASE("manifest migrate ignores directories and dotted files") {
  const scratch_home home;
  home.write(home.sandbox() / "skills" / "alpha" / "SKILL.md", "---\nkind: skill\n---\n");
  home.write(home.sandbox() / "skills" / ".link-manifest.json", "{}");
  const auto result = mf::migrate(home.sandbox(), false);
  REQUIRE(result.migrated.empty());
  REQUIRE(result.skipped.empty());
}

// --- the on-disk manifest format --------------------------------------------

TEST_CASE("manifest serialize_manifest reproduces the oracle's bytes") {
  // Verbatim from a real `.link-manifest.json`; see this file's header.
  mf::link_manifest value;
  value.version = 1;
  value.entries.push_back({"alpha",
                           "/tmp/pb/h/.planar/local/skills/alpha/SKILL.md",
                           {{"codex", "/tmp/pb/h/.codex/skills/local-alpha", "/tmp/pb/h/.planar/local/skills/alpha",
                             mf::mode::symlink, "2026-08-23T04:12:38Z"}}});

  REQUIRE(mf::serialize_manifest(value) == "{\n"
                                           "  \"version\": 1,\n"
                                           "  \"entries\": [\n"
                                           "    {\n"
                                           "      \"name\": \"alpha\",\n"
                                           "      \"source_path\": \"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md\",\n"
                                           "      \"links\": [\n"
                                           "        {\n"
                                           "          \"vendor\": \"codex\",\n"
                                           "          \"target_path\": \"/tmp/pb/h/.codex/skills/local-alpha\",\n"
                                           "          \"source_path\": \"/tmp/pb/h/.planar/local/skills/alpha\",\n"
                                           "          \"mode\": \"symlink\",\n"
                                           "          \"linked_at\": \"2026-08-23T04:12:38Z\"\n"
                                           "        }\n"
                                           "      ]\n"
                                           "    }\n"
                                           "  ]\n"
                                           "}\n");
}

TEST_CASE("manifest serialize_manifest renders an empty links array INLINE") {
  // Oracle capture of the agents manifest after a filtered link emptied the
  // entry: `"links": []` on one line, not an expanded `[\n      ]`.
  mf::link_manifest value;
  value.entries.push_back({"beta", "/tmp/pb/h/.planar/local/agents/beta.md", {}});
  REQUIRE(mf::serialize_manifest(value) == "{\n"
                                           "  \"version\": 1,\n"
                                           "  \"entries\": [\n"
                                           "    {\n"
                                           "      \"name\": \"beta\",\n"
                                           "      \"source_path\": \"/tmp/pb/h/.planar/local/agents/beta.md\",\n"
                                           "      \"links\": []\n"
                                           "    }\n"
                                           "  ]\n"
                                           "}\n");
}

TEST_CASE("manifest serialize_manifest renders an empty manifest") {
  REQUIRE(mf::serialize_manifest({}) == "{\n  \"version\": 1,\n  \"entries\": []\n}\n");
}

TEST_CASE("manifest serialize_manifest escapes the strings it writes") {
  // A skill name is operator-authored, so it can carry a quote or a backslash.
  mf::link_manifest value;
  value.entries.push_back({"q\"uote\\back", "/tmp/s", {}});
  REQUIRE(mf::serialize_manifest(value).find("\"name\": \"q\\\"uote\\\\back\"") != std::string::npos);
}

TEST_CASE("manifest serialize/parse round-trips") {
  mf::link_manifest value;
  value.entries.push_back({"alpha", "/s/a", {{"claude", "/t/a.md", "/s/a.md", mf::mode::copy, "T"}}});
  value.entries.push_back({"beta", "/s/b", {}});

  const auto reparsed = mf::parse_manifest(mf::serialize_manifest(value));
  REQUIRE(reparsed.has_value());
  REQUIRE(reparsed->entries.size() == 2);
  REQUIRE(reparsed->entries[0].links.at(0).mode == mf::mode::copy);
  REQUIRE(reparsed->entries[1].links.empty());
}

TEST_CASE("manifest parse_manifest normalises a missing version to 1") {
  const auto parsed = mf::parse_manifest("{\"entries\":[]}");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->version == 1);
}

TEST_CASE("manifest parse_manifest tolerates unknown keys") {
  // A manifest written by a NEWER Planar must stay readable, because
  // "unreadable" means "every install it records is orphaned".
  const auto parsed = mf::parse_manifest("{\"version\":1,\"entries\":[],\"future_field\":true}");
  REQUIRE(parsed.has_value());
}

TEST_CASE("manifest parse_manifest rejects malformed JSON") {
  REQUIRE_FALSE(mf::parse_manifest("{not json").has_value());
}

TEST_CASE("manifest load_manifest treats an ABSENT file as empty, not broken") {
  // The first-run state. Every caller depends on this NOT being an error, and
  // on it being distinguishable from a file that exists and does not parse.
  const scratch_home home;
  const auto         loaded = mf::load_manifest(home.root_, mf::kind::skill);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->entries.empty());
}

TEST_CASE("manifest load_manifest FAILS on a corrupt file") {
  const scratch_home home;
  home.write(mf::manifest_path(home.root_, mf::kind::skill), "{ this is not json");
  REQUIRE_FALSE(mf::load_manifest(home.root_, mf::kind::skill).has_value());
}

TEST_CASE("manifest save_manifest writes where load_manifest reads") {
  const scratch_home home;
  mf::link_manifest  value;
  value.entries.push_back({"alpha", "/s/a", {}});
  REQUIRE(mf::save_manifest(home.root_, mf::kind::agent, value));

  // Path is `<home>/.planar/local/agents/.link-manifest.json` — note it takes
  // the home dir, NOT the sandbox root, and appends the whole suffix itself.
  REQUIRE(std::filesystem::exists(home.sandbox() / "agents" / ".link-manifest.json"));
  const auto loaded = mf::load_manifest(home.root_, mf::kind::agent);
  REQUIRE(loaded->entries.size() == 1);
  // And it did NOT land in the skills manifest.
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.empty());
}

TEST_CASE("manifest save_manifest leaves no .tmp file behind") {
  const scratch_home home;
  REQUIRE(mf::save_manifest(home.root_, mf::kind::skill, {}));
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / ".link-manifest.json.tmp"));
}

// --- small helpers ----------------------------------------------------------

TEST_CASE("manifest parse_kind accepts singular and plural, defaulting to skill") {
  REQUIRE(mf::parse_kind(std::nullopt) == mf::kind::skill);
  REQUIRE(mf::parse_kind("skill") == mf::kind::skill);
  REQUIRE(mf::parse_kind("skills") == mf::kind::skill);
  REQUIRE(mf::parse_kind("agent") == mf::kind::agent);
  REQUIRE(mf::parse_kind("agents") == mf::kind::agent);
  REQUIRE_FALSE(mf::parse_kind("Skill").has_value());
  REQUIRE_FALSE(mf::parse_kind("").has_value());
}

TEST_CASE("manifest lookup_kind_for_name prefers a skill over an agent") {
  const scratch_home home;
  home.write_skill("both", "---\nkind: skill\n---\n");
  home.write_agent("both", "---\nkind: agent\n---\n");
  REQUIRE(mf::lookup_kind_for_name(home.sandbox(), "both") == mf::kind::skill);
}

TEST_CASE("manifest lookup_kind_for_name finds an agent and reports absence") {
  const scratch_home home;
  home.write_agent("solo", "---\nkind: agent\n---\n");
  REQUIRE(mf::lookup_kind_for_name(home.sandbox(), "solo") == mf::kind::agent);
  // nullopt is meaningful, not a failure: `local unlink` uses it to decide to
  // try BOTH manifests, which is how a deleted source still gets cleaned up.
  REQUIRE_FALSE(mf::lookup_kind_for_name(home.sandbox(), "absent").has_value());
}

TEST_CASE("manifest kind and mode names are the wire spellings") {
  REQUIRE(mf::kind_name(mf::kind::skill) == "skill");
  REQUIRE(mf::kind_name(mf::kind::agent) == "agent");
  REQUIRE(mf::kind_dir(mf::kind::skill) == "skills");
  REQUIRE(mf::kind_dir(mf::kind::agent) == "agents");
  REQUIRE(mf::mode_name(mf::mode::symlink) == "symlink");
  REQUIRE(mf::mode_name(mf::mode::copy) == "copy");
}
