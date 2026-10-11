// @file main.t.cpp
// @brief Standing ctest coverage for `surface_lint` (plan 996, task 6415).
//
// `surface_lint` (main.cpp, this directory) is a 1147-line deliberate C++
// port of `zig/tools/surface_lint.zig` (task 6402) enforcing the semantic
// half of the authored-surface gate — repository-relative links, retired
// references, artifact-set agreement, read-only capability drift, semantic
// command shapes, and the Agent Skills format. Before
// this file it had NO ctest-registered coverage: its sibling
// `cli_usage_lint` has `schema.t.cpp`'s `[lint-parity]` case; this tool had
// nothing (task 6402's coder disclosed the gap plainly, task 6415 files it).
//
// ## Why this expires
//
// `surface_lint`'s entire correctness argument to date is the differential
// against the zig original: task 6402 ran both over the same repo and got
// byte-identical results on clean-repo, seeded-violation, and a
// token-for-token diff of the 260-entry `command_classes` table. That
// argument DIES at M10 when `zig/` is deleted — after that there is no
// reference implementation and no standing gate of surface_lint's own. A
// regression here would be silent, in the one tool whose whole job is
// catching silent drift in authored surfaces.
//
// Every fixture below (src/tools/surface_lint/fixtures/) was authored
// against BOTH binaries during task 6415 — the zig oracle
// (`zig build surface-lint`) and this C++ port — and their outputs
// diffed byte-for-byte before this file was written. That differential
// cannot be re-run once `zig/` is gone; landing it now, while both sides
// still exist, is the entire point (task 6415's brief, verbatim).
//
// ## Fixtures that can fail
//
// The recurring defect shape this milestone hit repeatedly is a fixture
// whose setup cannot falsify what its test name claims — for a linter,
// that is a fixture with no violations, which cannot tell "found nothing"
// from "scanned nothing". Every fixture pair here is built to discriminate:
//
//   - dirty/    vs  clean/                 — six check classes fire vs. zero
//   - suppressed/                          — the SAME link-missing finding
//                                             the dirty fixture proves fires
//                                             is proven absent here, paired
//                                             so a suppression that always
//                                             (or never) matches is caught
//   - suppression_invalid/                 — six malformed-suppression
//                                             shapes AND two real findings
//                                             whose suppressions were
//                                             rejected, in the same file
//   - suppression_unused/                  — a syntactically valid
//                                             suppression matching no
//                                             finding
//   - capability_exemption/                — one exempted mutate shape
//                                             (silent) beside one
//                                             non-exempted mutate shape
//                                             (fires), same file, same role
//   - audit_selector/                      — a valid `--kind`/`--link`
//                                             selector is silent while a
//                                             selector missing its
//                                             required token fires, in the
//                                             same file (the OTHER clause
//                                             of check_command from
//                                             dirty/'s bare-token case)
//   - deferred_command/                    — the same `planar links
//                                             update` shape fires inline
//                                             AND fenced under agents/, and
//                                             is proven NOT gated under
//                                             docs/ in the same run
//   - skill_*/                             — the Agent Skills rules: one
//                                             valid skill passes with every
//                                             rule named, and one fixture per
//                                             failing rule names that rule
//   - retired_pending/                     — `/pl-` and `scriptorium` fire
//                                             only under
//                                             --enable-pending-retired
//   - vendor_excluded/                     — a file under agents/claude/
//                                             carrying three violations
//                                             that is never scanned at all
//                                             (files_scanned counts it OUT)
//
// ## Missing-binary handling FAILs, not SKIPs
//
// `surface_lint` is an unconditional add_subdirectory(src/tools) target
// (src/tools/CMakeLists.txt) and this test binary depends on it via
// add_dependencies (this directory's CMakeLists.txt). Its absence at test
// time means THIS test binary's own build is stale or broken, not that the
// tool is a legitimate absent environment dependency — task 6402 made the
// identical call for schema.t.cpp's `[lint-parity]` case (SKIP -> FAIL) for
// the identical reason: a SKIP here would let a broken build dependency
// edge hide behind a green ctest run.

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS

import std;

namespace {

namespace fs = std::filesystem;

/// @brief Run `bin arg...`, redirecting stdout+stderr to a scratch file
/// under the process temp dir, and return its contents plus the raw
/// `std::system` exit status. Named `planar_surface_lint_capture_*` so the
/// arena-sweep listener (cmake/test_support/arena_sweep_listener.cpp) reaps
/// it even on an aborted run.
auto capture(std::string const& bin, std::vector<std::string> const& args) -> std::pair<std::string, int> {
  auto const  out_path = fs::temp_directory_path() / std::format("planar_surface_lint_capture_{}.txt",
                                                                 std::chrono::steady_clock::now().time_since_epoch().count());
  std::string cmd_str  = "'" + bin + "'";
  for (auto const& arg : args) {
    cmd_str += " '" + arg + "'";
  }
  cmd_str += " > " + out_path.string() + " 2>&1";
  int const status = std::system(cmd_str.c_str());

  std::ifstream   in(out_path, std::ios::binary);
  std::string     contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::error_code ec;
  fs::remove(out_path, ec);
  return {contents, status};
}

/// @brief Count non-overlapping occurrences of `needle` in `haystack`.
auto count_occurrences(std::string_view haystack, std::string_view needle) -> std::size_t {
  std::size_t count  = 0;
  std::size_t cursor = 0;
  while (true) {
    auto const at = haystack.find(needle, cursor);
    if (at == std::string_view::npos)
      break;
    ++count;
    cursor = at + needle.size();
  }
  return count;
}

} // namespace

TEST_CASE("surface_lint enforces every check class against fixtures with provable violations", "[surface_lint][lint-parity]") {
  // `surface_lint` is an UNCONDITIONAL add_subdirectory(src/tools) target
  // (src/tools/CMakeLists.txt), and this test binary's CMakeLists.txt
  // declares an unconditional add_dependencies edge onto it. So a normal
  // build of THIS test binary guarantees the tool exists by the time any
  // SECTION below runs. Absence therefore means this test binary's own
  // build is stale or broken — FAIL, not SKIP (see this file's header).
  fs::path const bin{PLANAR_SURFACE_LINT_BIN};
  if (!fs::exists(bin)) {
    FAIL("surface_lint binary not built at "
         << bin.string()
         << " — surface_lint is an unconditional add_dependencies of surface_lint_tests (this directory's "
            "CMakeLists.txt); its absence means this test binary's own build is stale or broken, not that the "
            "tool is unavailable.");
  }
  fs::path const fixtures{PLANAR_SURFACE_LINT_FIXTURES_DIR};
  REQUIRE(fs::exists(fixtures));

  SECTION("clean corpus: zero findings, exit 0") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "clean").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out == "surface-lint: clean (1 files)\n");
  }

  SECTION("dirty corpus: link, legacy (x2), artifact-set, command, and capability all fire with exact diagnostics") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "dirty").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/drift.md:6: surface-link-missing: repository-relative link target does not exist: "
                 "missing-target.md\n"
                 "agents/drift.md:7: surface-legacy-reference: retired authored-surface reference: src/internal/\n"
                 "agents/drift.md:8: surface-legacy-reference: retired authored-surface reference: harness Agent/Task "
                 "tool\n"
                 "agents/drift.md:9: surface-artifact-set-drift: planning artifact set must contain product-spec, "
                 "tech-spec, roadmap, and test-spec\n"
                 "agents/drift.md:10: surface-command-drift: audit trail entity kind must use `--kind <kind> "
                 "<entity-id>`\n"
                 "agents/drift.md:11: surface-capability-drift: read-only role contains coordination or entity write: "
                 "planar artifact add\n"
                 "surface-lint: 6 finding(s) across 1 files\n");
  }

  SECTION("dirty corpus --json: envelope carries the same six findings") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "dirty").string(), "--json"});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.starts_with(R"({"version":1,"ok":false,"files_scanned":1,"findings":[)"));
    CHECK(count_occurrences(out, "\"code\":") == 6);
    CHECK(out.contains(R"("code":"surface-link-missing")"));
    CHECK(!out.contains("surface-contract-missing"));
  }

  SECTION("suppressed: a valid suppression consumes the exact finding it targets") {
    // Paired against the dirty-corpus section above: the SAME
    // surface-link-missing finding that section proves fires is proven
    // absent here. If suppression matching broke to always-consume or
    // never-consume, one half of this pair would flip.
    auto const [out, status] = capture(bin.string(), {(fixtures / "suppressed").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out == "surface-lint: clean (1 files)\n");
  }

  SECTION("suppression_invalid: all six malformed shapes are caught, and the two real findings they "
          "failed to suppress still fire") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "suppression_invalid").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/invalid.md:6: surface-suppression-invalid: file-wide suppressions are not allowed\n"
                 "agents/invalid.md:8: surface-suppression-invalid: expected `<!-- surface-lint-ignore <code>: "
                 "<rationale> -->`\n"
                 "agents/invalid.md:10: surface-suppression-invalid: suppression rationale is required after `:`\n"
                 "agents/invalid.md:12: surface-suppression-invalid: suppression names an unknown finding code\n"
                 "agents/invalid.md:13: surface-link-missing: repository-relative link target does not exist: "
                 "missing-target.md\n"
                 "agents/invalid.md:15: surface-suppression-invalid: suppression rationale must be non-empty\n"
                 "agents/invalid.md:16: surface-link-missing: repository-relative link target does not exist: "
                 "missing-target.md\n"
                 "agents/invalid.md:18: surface-suppression-invalid: suppression has no following non-blank line\n"
                 "surface-lint: 8 finding(s) across 1 files\n");
  }

  SECTION("suppression_unused: a syntactically valid suppression matching no finding is itself a finding") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "suppression_unused").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/unused.md:6: surface-suppression-unused: suppression for surface-link-missing does not match "
                 "a finding on the next non-blank line\n"
                 "surface-lint: 1 finding(s) across 1 files\n");
  }

  SECTION("audit_selector: a valid --kind selector is silent; a --kind or --link selector missing its "
          "required token is caught") {
    // Probes the OTHER clause of check_command from the dirty-corpus
    // section above: that one hits the bare-token branch
    // (`is_invalid_audit_selector`); this one hits the
    // `--kind`/`--link`-prefixed branch (`valid_audit_selector_args`). A
    // valid selector on line 6 stays silent while lines 8 and 10 (missing
    // entity id, missing link value) both fire, so a mutation that always
    // (or never) accepts the selector args moves this file's finding
    // count away from exactly 2.
    auto const [out, status] = capture(bin.string(), {(fixtures / "audit_selector").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/audit.md:8: surface-command-drift: audit trail selector requires `--kind <kind> "
                 "<entity-id>` or `--link <link-id>`\n"
                 "agents/audit.md:10: surface-command-drift: audit trail selector requires `--kind <kind> "
                 "<entity-id>` or `--link <link-id>`\n"
                 "surface-lint: 2 finding(s) across 1 files\n");
  }

  SECTION("capability_exemption: an exempted mutate shape is silent beside a non-exempted one that still fires") {
    // Paired within one file, one role: `planar plan create` is in
    // k_capability_exemptions for (agents/planar-introspector.md, introspector);
    // `planar decision add` is not. If the exemption table match broke to
    // always-exempt or never-exempt, this file's finding count would move
    // to 0 or 2 instead of staying at exactly 1.
    auto const [out, status] = capture(bin.string(), {(fixtures / "capability_exemption").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/planar-introspector.md:9: surface-capability-drift: read-only role contains coordination or entity "
                 "write: planar decision add\n"
                 "surface-lint: 1 finding(s) across 1 files\n");
  }

  SECTION("deferred_command: `planar links update` fires inline AND fenced under agents/, and is not gated "
          "under docs/") {
    // files_scanned == 2 (agents/deferred.md, docs/not-gated.md) but only
    // the agents/ file produces findings — proves the agents/+skills/
    // path gate in check_deferred_command, not just the check itself.
    auto const [out, status] = capture(bin.string(), {(fixtures / "deferred_command").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/deferred.md:6: surface-command-drift: deferred `planar links update` cannot be presented as "
                 "an executable current workflow; use unlink/link recovery\n"
                 "agents/deferred.md:9: surface-command-drift: deferred `planar links update` cannot be presented as "
                 "an executable current workflow; use unlink/link recovery\n"
                 "surface-lint: 2 finding(s) across 2 files\n");
  }

  SECTION("no feedback envelope rule: a skill without the seven headings is clean") {
    // The seven-section rule is gone (the contract lives in the skill's
    // references/feedback-contract.md). clean/docs/clean.md carries no
    // such heading and must still pass.
    auto const [out, status] = capture(bin.string(), {(fixtures / "clean").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(!out.contains("surface-contract-missing"));
  }

  SECTION("skill_valid: a valid Agent Skill passes and every rule is named on its own line") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_valid").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    for (auto rule : {"frontmatter-present", "name-matches-directory", "name-format", "description-length", "frontmatter-keys",
                      "body-lines", "links-resolve", "references-one-level"}) {
      INFO(rule);
      CHECK(count_occurrences(out, std::format("skills/planar/SKILL.md: skill-rule {}: ok", rule)) == 1);
    }
    CHECK(out.ends_with("surface-lint: clean (2 files)\n"));
  }

  SECTION("skill_name_mismatch: name `planr` in planar/ fails naming the mismatch") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_name_mismatch").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("skills/planar/SKILL.md:2: surface-skill-spec: name-matches-directory: name `planr` does not equal "
                       "directory `planar`\n"));
    CHECK(!out.contains("skill-rule name-matches-directory: ok"));
  }

  SECTION("skill_long_description: a 1025-character description fails, naming the length") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_long_description").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("skills/planar/SKILL.md:3: surface-skill-spec: description-length: description is 1025 characters; "
                       "the limit is 1024\n"));
  }

  SECTION("skill_deep_reference: references/deep/x.md fails the one-level rule") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_deep_reference").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("skills/planar/SKILL.md:8: surface-skill-spec: references-one-level: referenced file "
                       "`references/deep/x.md` is not one level below SKILL.md"));
    CHECK(out.contains("skill-rule links-resolve: ok"));
  }

  SECTION("skill_dangling_link: a link to a missing file fails the links rule once") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_dangling_link").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("skills/planar/SKILL.md:8: surface-skill-spec: links-resolve: link target does not exist: "
                       "references/missing.md\n"));
    CHECK(count_occurrences(out, "references/missing.md") == 1);
  }

  SECTION("skill_bad_key: a frontmatter key outside the Agent Skills set fails") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_bad_key").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("surface-skill-spec: frontmatter-keys: frontmatter key `slug` is not an Agent Skills key"));
  }

  SECTION("skill_151_lines: the lint allows 500 body lines, so a 151-line body passes") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "skill_151_lines").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out.contains("skill-rule body-lines: ok (151 of 500 lines)"));
  }

  SECTION("a 501-line body fails the lint's body-lines rule") {
    auto const tmp = fs::temp_directory_path() /
                     std::format("planar_surface_lint_body_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(tmp / "skills" / "planar");
    {
      std::ofstream f(tmp / "skills" / "planar" / "SKILL.md", std::ios::binary);
      f << "---\nname: planar\ndescription: Plan work.\n---\n";
      for (int i = 0; i < 501; ++i)
        f << "Line " << i << ".\n";
    }
    auto const [out, status] = capture(bin.string(), {tmp.string()});
    std::error_code ec;
    fs::remove_all(tmp, ec);
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.contains("surface-skill-spec: body-lines: body is 501 lines; the limit is 500"));
  }

  SECTION("retired_pending: switched off, /pl- and scriptorium pass; switched on they fail with file and line") {
    auto const [off, off_status] = capture(bin.string(), {(fixtures / "retired_pending").string()});
    INFO(off);
    REQUIRE(WIFEXITED(off_status));
    CHECK(WEXITSTATUS(off_status) == 0);
    CHECK(off == "surface-lint: clean (1 files)\n");

    auto const [on, on_status] = capture(bin.string(), {(fixtures / "retired_pending").string(), "--enable-pending-retired"});
    INFO(on);
    REQUIRE(WIFEXITED(on_status));
    CHECK(WEXITSTATUS(on_status) == 1);
    CHECK(on == "docs/pending.md:3: surface-retired-reference: retired reference outside an exempt region: /pl-\n"
                "docs/pending.md:5: surface-retired-reference: retired reference outside an exempt region: scriptorium\n"
                "surface-lint: 2 finding(s) across 1 files\n");
  }

  SECTION("vendor_excluded: agents/claude/ is never scanned, not merely scanned-and-clean") {
    // The fixture file carries three real violations (missing link,
    // retired reference, capability drift). files_scanned == 0 proves the
    // vendor-rendered directory is excluded at collection time, not that
    // it happened to pass every check.
    auto const [out, status] = capture(bin.string(), {(fixtures / "vendor_excluded").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out == "surface-lint: clean (0 files)\n");
  }

  SECTION("path_citation: a stale source-path citation fires; a real path, a suppressed "
          "placeholder, and a whole-file historical document all stay silent") {
    // docs/live.md carries three inline-code-span path citations: a
    // genuinely missing one (fires), a real one resolving against the
    // fixture tree's own src/real_file.cpp (silent — proves this isn't an
    // always-fire check), and an illustrative `[touches: ...]` syntax
    // example silenced by a `surface-lint-ignore surface-path-missing`
    // comment (silent — proves suppression applies to this new code).
    // docs/adrs.md cites another deleted path but is entirely silent via
    // the whole-file historical exemption (task 6930) — files_scanned == 2
    // proves it was still scanned, not excluded from collection.
    auto const [out, status] = capture(bin.string(), {(fixtures / "path_citation").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "docs/live.md:3: surface-path-missing: authored surface cites a repository path that does not "
                 "resolve against the working tree: src/ghost_file.cpp\n"
                 "surface-lint: 1 finding(s) across 2 files\n");
  }

  SECTION("path_citation_bare: a non-resolving path with NO backticks around it does not fire") {
    // Pins the deliberate scope boundary documented in checkPathCitations's
    // module doc (task 6930): only inline CODE-SPAN citations are checked.
    // A bare, unformatted path mention in plain prose is out of scope by
    // design, not by accident — this fixture is where a future author who
    // wants to move that boundary will find the decision recorded.
    auto const [out, status] = capture(bin.string(), {(fixtures / "path_citation_bare").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out == "surface-lint: clean (1 files)\n");
  }

  SECTION("claude_md: CLAUDE.md is scanned; its AGENTS.md symlink is not scanned a second time") {
    // Task 6930's brief names CLAUDE.md explicitly in scope, but it is a
    // single top-level file, not a directory under k_scan_dirs, and
    // (unlike every other in-scope surface) has no dedicated
    // collect_markdown() walk. This fixture's AGENTS.md is a REAL symlink
    // to CLAUDE.md, same as this repo's own root: if scan_repository ever
    // started walking symlinked root files as well, the single stale
    // citation below would be reported twice, once under each name, and
    // files_scanned would read 2 instead of 1.
    auto const [out, status] = capture(bin.string(), {(fixtures / "claude_md").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "CLAUDE.md:3: surface-path-missing: authored surface cites a repository path that does not "
                 "resolve against the working tree: src/ghost_claude.cpp\n"
                 "surface-lint: 1 finding(s) across 1 files\n");
  }

  SECTION("queue_command: a span or block line that begins with a build or test program fires; queued, "
          "prose, marked, region-exempt and out-of-scope text stays silent") {
    // One file carries every shape: a span, a fenced line, an indented fenced
    // line behind a `$ ` prompt, two spans on one line, a queued span and a
    // queued block line (silent), prose naming programs (silent), non-build
    // `cmake --preset` / `make:` / `go vet` / `npm install` spans (silent), a
    // line carrying `queue-lint-ignore` (silent), a begin/end region holding a
    // span, a span line and a fenced line (silent), and a span right after the
    // region (fires again). docs/ is not gated; skills/ is.
    auto const [out, status] = capture(bin.string(), {(fixtures / "queue_command").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/direct.md:6: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `make test`\n"
                 "agents/direct.md:9: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `ctest --test-dir build`\n"
                 "agents/direct.md:13: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `$ cmake --build build/debug`\n"
                 "agents/direct.md:16: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `cargo test --release`\n"
                 "agents/direct.md:16: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `ninja`\n"
                 "agents/direct.md:35: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `gradle build`\n"
                 "skills/skill/SKILL.md:6: surface-queue-command: build or test command is not given to `planar-agent "
                 "queue run`: `pytest -q`\n"
                 "skills/skill/SKILL.md: skill-rule frontmatter-present: ok (closed at line 4)\n"
                 "skills/skill/SKILL.md: skill-rule name-matches-directory: ok (skill)\n"
                 "skills/skill/SKILL.md: skill-rule name-format: ok (skill)\n"
                 "skills/skill/SKILL.md: skill-rule description-length: ok (49 of 1024 characters)\n"
                 "skills/skill/SKILL.md: skill-rule frontmatter-keys: ok (2 keys)\n"
                 "skills/skill/SKILL.md: skill-rule body-lines: ok (2 of 500 lines)\n"
                 "skills/skill/SKILL.md: skill-rule links-resolve: ok (every relative link exists)\n"
                 "skills/skill/SKILL.md: skill-rule references-one-level: ok (every referenced file is one level "
                 "below SKILL.md)\n"
                 "surface-lint: 7 finding(s) across 3 files\n");
  }

  SECTION("queue_marker_invalid: an end without a begin, a begin inside a region and a region never closed are "
          "reported, and the region state still decides what is exempt") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "queue_marker_invalid").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/markers.md:5: surface-queue-marker-invalid: `queue-lint-ignore-end` without a matching "
                 "`queue-lint-ignore-begin`\n"
                 "agents/markers.md:6: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `make`\n"
                 "agents/markers.md:8: surface-queue-marker-invalid: `queue-lint-ignore-begin` inside the region opened "
                 "at line 7\n"
                 "agents/markers.md:11: surface-queue-command: build or test command is not given to `planar-agent queue "
                 "run`: `ninja`\n"
                 "agents/markers.md:12: surface-queue-marker-invalid: `queue-lint-ignore-begin` is never closed\n"
                 "surface-lint: 5 finding(s) across 1 files\n");
  }

  SECTION("retired_refs: scoped hits fire, in a fence too; the changelog, the marked INSTALL.md region and "
          "unscanned out-of-scope files stay silent") {
    // One tree carries every shape the retired-reference lint distinguishes
    // (plan 1089, tech spec 656 § Retired-reference lint). SCOPED hits: an
    // inline `agent.db` and a `PLANAR_AGENT_DB` in docs/scoped.md, an
    // `agent.db` inside a fenced block there, a `limit_columns_select` in the
    // root README.md, and a `migrations-agent` in INSTALL.md AFTER its marked
    // region. EXEMPT hits: docs/changelog.md, and INSTALL.md between its two
    // markers (the unmarked INSTALL.md line 3 fires beside it, so the exemption
    // is the region and not the file). OUT-OF-SCOPE hits: migrations/README.md
    // and scripts/notes.md name the terms freely. files_scanned == 4 proves
    // those two were never read, and a stale mention in them cannot be what
    // keeps the output exact.
    auto const [out, status] = capture(bin.string(), {(fixtures / "retired_refs").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "INSTALL.md:3: surface-retired-reference: retired reference outside an exempt region: agent.db\n"
                 "INSTALL.md:11: surface-retired-reference: retired reference outside an exempt region: migrations-agent\n"
                 "README.md:3: surface-retired-reference: retired reference outside an exempt region: limit_columns_select\n"
                 "docs/scoped.md:3: surface-retired-reference: retired reference outside an exempt region: agent.db\n"
                 "docs/scoped.md:5: surface-retired-reference: retired reference outside an exempt region: "
                 "PLANAR_AGENT_DB\n"
                 "docs/scoped.md:8: surface-retired-reference: retired reference outside an exempt region: agent.db\n"
                 "surface-lint: 6 finding(s) across 4 files\n");
  }

  SECTION("retired_marker_invalid: an unclosed region and a marker outside INSTALL.md are findings, and the "
          "stray marker silences nothing") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "retired_marker_invalid").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "INSTALL.md:3: surface-retired-ref-marker-invalid: the retired-ref region opened here is never "
                 "closed\n"
                 "docs/stray.md:3: surface-retired-ref-marker-invalid: a retired-ref marker is honoured only in "
                 "INSTALL.md\n"
                 "docs/stray.md:4: surface-retired-reference: retired reference outside an exempt region: agent.db\n"
                 "docs/stray.md:5: surface-retired-ref-marker-invalid: a retired-ref marker is honoured only in "
                 "INSTALL.md\n"
                 "surface-lint: 4 finding(s) across 2 files\n");
  }

  SECTION("--command-inventory-json: the pinned 260-entry command_classes table") {
    auto const [out, status] = capture(bin.string(), {"--command-inventory-json"});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out.starts_with(R"({"version":1,"commands":[)"));
    CHECK(count_occurrences(out, "\"command\":") == 260);
    CHECK(out.contains(R"({"command":"planar init","access":"mutate"})"));
    CHECK(count_occurrences(out, "\"access\":\"read\"") + count_occurrences(out, "\"access\":\"mutate\"") == 260);
  }

  SECTION("usage error: no repo-root argument is a usage error, not a crash") {
    auto const [out, status] = capture(bin.string(), {});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 2);
    CHECK(out.contains("usage: surface_lint"));
  }
}

namespace {

/// @brief Body lines of a SKILL.md: the lines after the closing `---` of its
/// frontmatter, not counting the final newline.
auto skill_body_lines(fs::path const& file) -> std::size_t {
  std::ifstream in(file, std::ios::binary);
  std::string   line;
  std::size_t   fences = 0;
  std::size_t   body   = 0;
  while (std::getline(in, line)) {
    if (fences >= 2) {
      ++body;
    } else if (line == "---") {
      ++fences;
    }
  }
  return body;
}

constexpr std::size_t k_planar_skill_body_budget = 150;

} // namespace

// Planar's own budget for the entry skill is tighter than the Agent Skills
// limit of 500, so it is a repository contract and not a lint rule.
TEST_CASE("the body line counter counts a padded fixture past the Planar budget", "[surface_lint][skill-budget]") {
  fs::path const fixtures{PLANAR_SURFACE_LINT_FIXTURES_DIR};
  auto const     count = skill_body_lines(fixtures / "skill_151_lines" / "skills" / "planar" / "SKILL.md");
  CHECK(count == 151);
  CHECK(count > k_planar_skill_body_budget);
  CHECK(skill_body_lines(fixtures / "skill_valid" / "skills" / "planar" / "SKILL.md") <= k_planar_skill_body_budget);
}

TEST_CASE("skills/planar/SKILL.md body stays within 150 lines", "[surface_lint][skill-budget]") {
  fs::path const skill = fs::path{PLANAR_REPO_ROOT} / "skills" / "planar" / "SKILL.md";
  if (!fs::exists(skill)) {
    SKIP("skills/planar/SKILL.md does not exist in this checkout yet; the 150-line budget is checked once it lands");
  }
  auto const count = skill_body_lines(skill);
  INFO("skills/planar/SKILL.md body is " << count << " lines; the Planar budget is " << k_planar_skill_body_budget);
  CHECK(count <= k_planar_skill_body_budget);
}
