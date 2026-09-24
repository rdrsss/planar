// @file main.t.cpp
// @brief Standing ctest coverage for `surface_lint` (plan 996, task 6415).
//
// `surface_lint` (main.cpp, this directory) is a 1147-line deliberate C++
// port of `zig/tools/surface_lint.zig` (task 6402) enforcing the semantic
// half of the authored-surface gate — repository-relative links, retired
// references, artifact-set agreement, read-only capability drift, semantic
// command shapes, and the seven-heading feedback/recovery contract. Before
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
//   - internal_only/                       — a skill with none of the
//                                             seven required headings that
//                                             is exempt via frontmatter,
//                                             paired against dirty/'s
//                                             missing-contract.md (same
//                                             absence, no exemption, fires)
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
                 "skills/src/missing-contract.md:1: surface-contract-missing: user-invocable skill is missing required "
                 "feedback heading `## Recovery`; add that literal H2 section or declare a genuine helper as "
                 "`internal_only: true` in frontmatter\n"
                 "surface-lint: 7 finding(s) across 2 files\n");
  }

  SECTION("dirty corpus --json: envelope carries the same seven findings") {
    auto const [out, status] = capture(bin.string(), {(fixtures / "dirty").string(), "--json"});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.starts_with(R"({"version":1,"ok":false,"files_scanned":2,"findings":[)"));
    CHECK(count_occurrences(out, "\"code\":") == 7);
    CHECK(out.contains(R"("code":"surface-link-missing")"));
    CHECK(out.contains(R"("code":"surface-contract-missing")"));
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
    // k_capability_exemptions for (agents/introspector.md, introspector);
    // `planar decision add` is not. If the exemption table match broke to
    // always-exempt or never-exempt, this file's finding count would move
    // to 0 or 2 instead of staying at exactly 1.
    auto const [out, status] = capture(bin.string(), {(fixtures / "capability_exemption").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out == "agents/introspector.md:9: surface-capability-drift: read-only role contains coordination or entity "
                 "write: planar decision add\n"
                 "surface-lint: 1 finding(s) across 1 files\n");
  }

  SECTION("deferred_command: `planar links update` fires inline AND fenced under agents/, and is not gated "
          "under docs/") {
    // files_scanned == 2 (agents/deferred.md, docs/not-gated.md) but only
    // the agents/ file produces findings — proves the agents/+skills/src/
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

  SECTION("internal_only: a skill missing all seven feedback headings is exempt via frontmatter") {
    // Paired against dirty/skills/src/missing-contract.md above: the same
    // absence (no `## Recovery` etc.) fires there and is silent here,
    // solely because of `internal_only: true`.
    auto const [out, status] = capture(bin.string(), {(fixtures / "internal_only").string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out == "surface-lint: clean (1 files)\n");
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
