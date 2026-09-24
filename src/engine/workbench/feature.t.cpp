// @file feature.t.cpp
// @brief Unit tests for `planar.engine.workbench.feature` (plan 996, task
// 6037): the directory layout, filename derivation, and path confinement.
//
// ORACLE PROVENANCE. The layout was read off a real `workbench push` against
// a scratch root, association slug `project:demo`, plan slug `demo-feature`,
// no external link (so the plan key is the `p<id>` fallback):
//
//   <root>/project_demo/p1-demo-feature/README.md
//   <root>/project_demo/p1-demo-feature/1-tech-spec-auth.md
//   <root>/project_demo/p1-demo-feature/plans/child-ms.md
//   <root>/project_demo/p1-demo-feature/tasks/cross/1-first-task.md
//   <root>/project_demo/p1-demo-feature/decisions/1-use-sqlite.md
//   <root>/project_demo/p1-demo-feature/questions/1-which-format.md
//   <root>/project_demo/p1-demo-feature/scenarios/1-round-trip.md
//   <root>/project_demo/p1-demo-feature/.sync
//
// Note the ARTIFACT sits at the feature-dir ROOT, not under `artifacts/`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.feature;

namespace {

namespace wf = planar::engine::workbench::feature;

} // namespace

TEST_CASE("feature_dir composes root / assoc / <key>-<slug>", "[workbench][feature]") {
  CHECK(wf::feature_dir("/wb", "project-checkout", "p42", "my-plan") == "/wb/project-checkout/p42-my-plan");
}

TEST_CASE("a colon in the association slug becomes an underscore", "[workbench][feature]") {
  // Association slugs are `kind:name`, so this is the ordinary case, not an
  // edge one -- `project:demo` is what the oracle run above used.
  CHECK(wf::feature_dir("/wb", "project:demo", "p1", "demo-feature") == "/wb/project_demo/p1-demo-feature");
}

TEST_CASE("a BACKSLASH and an embedded NUL in the association slug become an underscore too", "[workbench][feature][safety]") {
  // safe_assoc_slug shares its character set with safe_path_segment
  // (plus `:`), but is a SEPARATE function with its own copy of the
  // check -- the colon fixture above never exercises backslash or NUL.
  CHECK(wf::feature_dir("/wb", "a\\b", "p1", "s") == "/wb/a_b/p1-s");
  CHECK(wf::feature_dir("/wb", std::string_view("a\0b", 3), "p1", "s") == "/wb/a_b/p1-s");
}

TEST_CASE("an EMPTY association collapses the directory level entirely", "[workbench][feature]") {
  // Global-scope plans. An empty component must vanish rather than become
  // `_`, or every global feature would live under a literal `_` directory.
  CHECK(wf::feature_dir("/wb", "", "p1", "root-plan") == "/wb/p1-root-plan");
}

TEST_CASE("a root with a trailing slash does not double the separator", "[workbench][feature]") {
  CHECK(wf::feature_dir("/wb/", "a", "p1", "s") == "/wb/a/p1-s");
}

TEST_CASE("traversal segments are confined to one literal directory name", "[workbench][feature][safety]") {
  // The load-bearing case. This module builds a path out of database- and
  // external-system-supplied strings, and `archive` DELETES the result. A
  // `..` that survived as a real path component would delete outside the
  // workbench root.
  CHECK(wf::feature_dir("/wb", "org", "../../target", "plan") == "/wb/org/.._.._target-plan");
  CHECK(wf::feature_dir("/wb", "..", "p1", "s") == "/wb/__/p1-s");
  CHECK(wf::feature_dir("/wb", ".", "p1", "s") == "/wb/_/p1-s");
  CHECK(wf::feature_dir("/wb", "a/../b", "p1", "s") == "/wb/a_.._b/p1-s");
  // A `.` or `..` as the plan KEY or SLUG likewise cannot escape.
  CHECK(wf::feature_dir("/wb", "org", "..", "s") == "/wb/org/_-s");
  CHECK(wf::feature_dir("/wb", "org", "p1", "..") == "/wb/org/p1-_");
  // A bare `.` as the plan KEY or SLUG (as opposed to `..`, or ASSOC's `.`
  // above, which goes through the separate `safe_assoc_slug`) is its own
  // clause of `safe_path_segment`'s guard. Closes a break-probe SURVIVOR
  // (task 6423): no existing fixture ever put a literal `.` through
  // `safe_path_segment` itself.
  CHECK(wf::feature_dir("/wb", "org", ".", "s") == "/wb/org/_-s");
  CHECK(wf::feature_dir("/wb", "org", "p1", ".") == "/wb/org/p1-_");
  // An EMPTY plan key or slug is the third clause, and -- unlike an empty
  // ASSOC, which collapses the directory level -- must become `_`, not
  // vanish, or `feature_name`'s `<key>-<slug>` would silently lose a side.
  // Also closes a break-probe SURVIVOR (task 6423).
  CHECK(wf::feature_dir("/wb", "org", "", "s") == "/wb/org/_-s");
  CHECK(wf::feature_dir("/wb", "org", "p1", "") == "/wb/org/p1-_");
}

TEST_CASE("a BACKSLASH and an embedded NUL are sanitized like a slash", "[workbench][feature][safety]") {
  // safe_path_segment's character check is `ch == '/' || ch == '\\' || ch
  // == '\0'` -- a Windows-style traversal separator and an embedded NUL are
  // just as load-bearing for this bucket's own "highest-risk" claim as `/`
  // and `..`, but no fixture in this module ever constructed either one.
  CHECK(wf::feature_dir("/wb", "org", "p1", "a\\b") == "/wb/org/p1-a_b");
  CHECK(wf::feature_dir("/wb", "org", "a\\..\\b", "s") == "/wb/org/a_.._b-s");
  CHECK(wf::feature_dir("/wb", "org", "p1", std::string_view("a\0b", 3)) == "/wb/org/p1-a_b");
}

TEST_CASE("a hierarchical external plan key stays one directory name", "[workbench][feature]") {
  // GitHub-style keys really do contain a slash, and JIRA-style ones do not.
  CHECK(wf::feature_dir("/wb", "org", "owner/repo#1", "plan") == "/wb/org/owner_repo#1-plan");
  CHECK(wf::feature_dir("/home/u/.planar/workbench", "org:eng", "JIRA-123", "add-auth") ==
        "/home/u/.planar/workbench/org_eng/JIRA-123-add-auth");
}

TEST_CASE("stored_path is the same layout, root-relative", "[workbench][feature]") {
  CHECK(wf::stored_path("project:checkout", "p42", "my-plan", "tasks/cross/5-t.md") ==
        "project_checkout/p42-my-plan/tasks/cross/5-t.md");
  CHECK(wf::stored_path("", "p1", "root-plan", "README.md") == "p1-root-plan/README.md");
}

// --- slugify --------------------------------------------------------------

TEST_CASE("slugify lowercases and collapses non-alphanumeric runs", "[workbench][feature][slug]") {
  CHECK(wf::slugify("Tech Spec: Auth") == "tech-spec-auth");
  CHECK(wf::slugify("  Hello World!  ") == "hello-world");
  CHECK(wf::slugify("Plan 226 v2") == "plan-226-v2");
  CHECK(wf::slugify("a___b") == "a-b");
}

TEST_CASE("slugify returns `untitled` for an empty result", "[workbench][feature][slug]") {
  CHECK(wf::slugify("") == "untitled");
  CHECK(wf::slugify("---") == "untitled");
  CHECK(wf::slugify("!@#$") == "untitled");
}

TEST_CASE("slugify treats every byte of a multi-byte character as a separator", "[workbench][feature][slug]") {
  // Byte-wise on purpose: normalizing Unicode here would change filenames
  // the oracle already wrote. `Héllo` is h + (2 separator bytes) + llo, and
  // the run collapses to one dash.
  CHECK(wf::slugify("Héllo Wörld") == "h-llo-w-rld");
}

TEST_CASE("slugify truncates to 60 and re-trims a dash the cut exposed", "[workbench][feature][slug]") {
  // The SECOND trim is the subtle half: without it a title that happens to
  // break on a separator yields a filename ending in `-`.
  auto const long_run = wf::slugify("abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz012");
  CHECK(long_run.size() == 60);
  CHECK(long_run.back() != '-');

  // The cut must land ON the separator for the re-trim to matter, and
  // getting that arithmetic wrong is how the first draft of this case
  // passed against an implementation with the second trim DELETED. 59 a's,
  // a separator, then more: the slug is 59 a's + '-' + "bbb" (63 chars), so
  // truncating to 60 leaves 59 a's plus a TRAILING DASH, and only the
  // second trim removes it.
  std::string const at_boundary = std::string(59, 'a') + "!bbb";
  CHECK(wf::slugify(at_boundary).size() == 63 - 3 - 1); // sanity: the pre-trim slug is 63 chars
  auto const cut = wf::slugify(at_boundary);
  CHECK(cut.back() != '-');
  CHECK(cut == std::string(59, 'a'));
}

// --- artifact filenames ---------------------------------------------------

TEST_CASE("artifact_filename is <id>-<slug>.md", "[workbench][feature]") {
  CHECK(wf::artifact_filename(7, "Tech Spec: Auth", "tech_spec") == "7-tech-spec-auth.md");
  CHECK(wf::artifact_filename(99, "My Roadmap", "roadmap") == "99-my-roadmap.md");
}

TEST_CASE("an unusable title falls back to the KIND, with underscores rewritten", "[workbench][feature]") {
  CHECK(wf::artifact_filename(3, "", "tech_spec") == "3-tech-spec.md");
  CHECK(wf::artifact_filename(3, "!!!", "product_spec") == "3-product-spec.md");
  // A title that slugifies to the literal `untitled` also triggers the
  // fallback -- the check is on the RESULT, not on the input being empty.
  CHECK(wf::artifact_filename(3, "Untitled", "adr") == "3-adr.md");
}

TEST_CASE("canonical_entity_kind aliases only test_scenario", "[workbench][feature]") {
  CHECK(wf::canonical_entity_kind("test_scenario") == "scenario");
  CHECK(wf::canonical_entity_kind("scenario") == "scenario");
  CHECK(wf::canonical_entity_kind("task") == "task");
  CHECK(wf::canonical_entity_kind("plan") == "plan");
  CHECK(wf::canonical_entity_kind("artifact") == "artifact");
}
