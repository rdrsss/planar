// @file touchinfer.t.cpp
// @brief Engine-level tests for `planar.engine.planning.touchinfer` (plan
// 996, task 6330).
//
// The CLI-visible bytes are pinned in
// `src/cmd/planar/task_touches_infer_leaf.t.cpp`. This file covers the
// extraction and resolution halves directly, where a leaf test would have to
// reach them through rendering.
//
// ## EVERY FIXTURE ASSERTS ITS OWN SHAPE BEFORE ANYTHING IS COMPARED
//
// This is a HEURISTIC over derived data, which makes it unusually easy to
// test nothing at all: a fixture tree that was never actually written to
// disk resolves every token to `unresolved`, and a test that only asserts
// "this token is not `resolved`" passes on the empty tree exactly as it
// passes on the right one. `the fixture tree is on disk as described` below
// runs FIRST and asserts the tree's own shape — every file this file's later
// cases depend on, by path, with its parent's kind — so a silently-empty
// arena fails there rather than being reported as a green heuristic.
//
// Every absence assertion is paired with the presence case that shares its
// mechanism: `src/imaginary.zig` is `unresolved` in the same call in which
// `src/engine/a.zig` is `resolved`, `pkg/.git/config` is absent from an
// expansion that DOES contain `pkg/real.zig`, and so on. An absence alone
// would not distinguish "the rule fired" from "the walk found nothing".
//
// ## ORACLE PROVENANCE
//
// Every classification, path set and count below was captured by RUNNING
// `zig/zig-out/bin/planar` built at this cycle's base against a seeded
// scratch arena (`PLANAR_DB` under a temp root), every invocation through a
// pipe on both streams with the exit code read outside it. The fixture tree
// this file builds is the same tree that capture ran against, file for file.
//
// Nine tasks were captured, one per arm — resolved+unresolved, directory,
// multi-match basename, over-cap, no-token, empty-body, hidden-entry,
// empty-directory, and citation/URL/absolute/traversal — in text, `--json`
// and `--json --wide` form, plus `--apply` idempotence and the four error
// arms. The captures are quoted inline at each assertion that pins one.
//
// ## THE ONE DELIBERATE DIVERGENCE
//
// Expansion ORDER. The oracle emits readdir order; this port sorts. The
// captured oracle bytes for the `docs/` token were
// `["docs/b.md","docs/sub/c.md","docs/a.md"]` — APFS hash order, not a rule
// — so `a directory token expands to its files` below asserts the SORTED
// list and says so at the assertion. See touchinfer.cppm's DIVERGENCE
// section for why order cannot reach state.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.touchinfer;

namespace {

namespace ti = planar::engine::planning::touchinfer;

/// @brief A scratch directory tree, removed on destruction.
struct scratch_tree {
  std::filesystem::path root_;
  scratch_tree()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_touchinfer_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(root_);
  }
  scratch_tree(const scratch_tree&)            = delete;
  scratch_tree& operator=(const scratch_tree&) = delete;
  ~scratch_tree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
};

/// @brief Write a file, creating its parents.
void put(const std::filesystem::path& root, std::string_view rel, std::string_view data = "x") {
  const auto p = root / std::filesystem::path(rel);
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << data;
}

/// @brief Build the arena the oracle capture ran against.
///
/// Deliberately NOT derived from any constant this file asserts: the paths
/// are written out literally here and read back literally in the shape
/// assertion, so a typo shows as a failure rather than agreeing with itself.
void build_fixture(const std::filesystem::path& root) {
  put(root, "src/engine/a.zig");
  put(root, "docs/a.md");
  put(root, "docs/b.md");
  put(root, "docs/sub/c.md");
  put(root, "a/deep/task.zig");
  put(root, "b/task.zig");
  put(root, "pkg/.git/config");
  put(root, "pkg/real.zig");
  std::filesystem::create_directories(root / "emptydir");
  // 70 files: over `k_max_directory_expansion` (64), so `big/` is the
  // `too_broad` arm rather than a large `directory` one.
  for (int i = 1; i <= 70; ++i) {
    put(root, std::format("big/f{}.txt", i));
  }
}

auto classify(const std::filesystem::path& root, std::string_view token) {
  return ti::classify_token(root, token);
}

} // namespace

// =========================================================================
// The fixture's own shape — runs before anything is compared
// =========================================================================

TEST_CASE("the fixture tree is on disk as described", "[engine][planning][touchinfer][fixture]") {
  scratch_tree tree;
  build_fixture(tree.root_);
  const auto& r = tree.root_;

  // Files, by path. If the arena silently failed to materialise, every
  // resolution test below would report `unresolved` and pass vacuously.
  for (auto const* rel : {"src/engine/a.zig", "docs/a.md", "docs/b.md", "docs/sub/c.md", "a/deep/task.zig", "b/task.zig",
                          "pkg/real.zig", "pkg/.git/config"}) {
    INFO("fixture file: " << rel);
    REQUIRE(std::filesystem::is_regular_file(r / rel));
  }

  // Directories, including the two whose EMPTINESS and SIZE are the point.
  REQUIRE(std::filesystem::is_directory(r / "docs"));
  REQUIRE(std::filesystem::is_directory(r / "docs/sub"));
  REQUIRE(std::filesystem::is_directory(r / "pkg/.git"));
  REQUIRE(std::filesystem::is_directory(r / "emptydir"));
  REQUIRE(std::filesystem::is_empty(r / "emptydir"));

  // `big/` must be strictly OVER the cap or the `too_broad` case degrades
  // into an ordinary `directory` one without any test noticing.
  const auto big_count = static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(r / "big"), std::filesystem::directory_iterator{}));
  REQUIRE(big_count == 70);
  REQUIRE(big_count > ti::k_max_directory_expansion);

  // And `docs/` must be strictly UNDER it, so the two arms are separated by
  // the cap rather than by accident.
  REQUIRE(std::size_t{3} < ti::k_max_directory_expansion);
}

// =========================================================================
// Token extraction — pure, no filesystem
// =========================================================================

TEST_CASE("is_path_shaped accepts repo-relative paths and rejects prose", "[engine][planning][touchinfer]") {
  CHECK(ti::is_path_shaped("src/engine/planning/touchinfer.zig"));
  CHECK(ti::is_path_shaped("docs/cli-reference.md"));
  CHECK(ti::is_path_shaped("workflows/"));
  CHECK(ti::is_path_shaped("strategy.zig"));

  // Prose, URLs, absolute paths, traversal — none are repo touches.
  CHECK_FALSE(ti::is_path_shaped("the"));
  CHECK_FALSE(ti::is_path_shaped("serialize"));
  CHECK_FALSE(ti::is_path_shaped("https://example.com/a.zig"));
  CHECK_FALSE(ti::is_path_shaped("/etc/passwd"));
  CHECK_FALSE(ti::is_path_shaped("../outside.zig"));
  CHECK_FALSE(ti::is_path_shaped(""));

  // The extension rule is INTERIOR-dot, not any-dot: a leading dot alone
  // does not qualify a separator-free token, and a trailing one does not
  // either. Neither case is reachable through `extract_tokens` (which
  // strips trailing dots first), which is exactly why it is pinned here.
  CHECK_FALSE(ti::is_path_shaped(".zig"));
  CHECK_FALSE(ti::is_path_shaped("strategy."));

  // Over-length is rejected; one byte under the cap is not.
  CHECK(ti::is_path_shaped(std::string(511, 'a') + "/"));
  CHECK_FALSE(ti::is_path_shaped(std::string(513, 'a') + "/"));
}

TEST_CASE("strip_line_suffix handles the codebase's path:line citation style", "[engine][planning][touchinfer]") {
  CHECK(ti::strip_line_suffix("docs/cli-reference.md:339") == "docs/cli-reference.md");
  CHECK(ti::strip_line_suffix("src/engine/x.zig:12-40") == "src/engine/x.zig");

  // Not a line reference — leave it alone.
  CHECK(ti::strip_line_suffix("docs/a.md") == "docs/a.md");
  CHECK(ti::strip_line_suffix("src/a.zig:name") == "src/a.zig:name");
  CHECK(ti::strip_line_suffix(":") == ":");

  // The LAST colon is the split point, so a Windows-ish double colon keeps
  // its head. Pinned because "find the colon" has two plausible readings.
  CHECK(ti::strip_line_suffix("a:b:12") == "a:b");
}

TEST_CASE("extract_tokens lifts paths out of prose and strips punctuation", "[engine][planning][touchinfer]") {
  const auto toks = ti::extract_tokens("Delete `workflows/status.lua` and workflows/health.lua, then drop\n"
                                       "the import at integration_tests/all_test.zig (line 108).");
  REQUIRE(toks.size() == 3);
  CHECK(toks[0] == "workflows/status.lua");
  CHECK(toks[1] == "workflows/health.lua");
  CHECK(toks[2] == "integration_tests/all_test.zig");
}

TEST_CASE("extract_tokens recovers paths cited with line numbers", "[engine][planning][touchinfer]") {
  const auto toks = ti::extract_tokens("See CLAUDE.md:339 and src/engine/planning/strategy.zig:64-66.");
  REQUIRE(toks.size() == 2);
  CHECK(toks[0] == "CLAUDE.md");
  CHECK(toks[1] == "src/engine/planning/strategy.zig");
}

TEST_CASE("extract_tokens recovers a path ending a sentence", "[engine][planning][touchinfer]") {
  // Regression the oracle carries a note for: the trailing period rode along
  // with the token and the path resolved as `unresolved` — a silent
  // under-declaration in the most natural phrasing there is.
  const auto toks = ti::extract_tokens("Rework src/alpha.zig. Then check docs/b.md:12.");
  REQUIRE(toks.size() == 2);
  CHECK(toks[0] == "src/alpha.zig");
  CHECK(toks[1] == "docs/b.md");
}

TEST_CASE("extract_tokens ignores prose that merely contains dots", "[engine][planning][touchinfer]") {
  CHECK(ti::extract_tokens("Run it. Then verify. No paths here!").empty());
}

TEST_CASE("extract_tokens keeps duplicates; dedup is infer_from_text's job", "[engine][planning][touchinfer]") {
  // The split of responsibility matters: dedup is per-TASK, across fields,
  // so it cannot happen here. A token twice in one field is two tokens.
  const auto toks = ti::extract_tokens("src/a.zig and src/a.zig again");
  REQUIRE(toks.size() == 2);
  CHECK(toks[0] == "src/a.zig");
  CHECK(toks[1] == "src/a.zig");
}

// =========================================================================
// Filesystem resolution
// =========================================================================

TEST_CASE("an exact file token resolves to itself", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const [cls, paths] = classify(tree.root_, "src/engine/a.zig");
  CHECK(cls == ti::classification::resolved);
  REQUIRE(paths.size() == 1);
  CHECK(paths[0] == "src/engine/a.zig");
}

TEST_CASE("a directory token expands to its files rather than dropping", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const [cls, paths] = classify(tree.root_, "docs/");
  CHECK(cls == ti::classification::directory);
  // Recursive: the nested file counts too. Wider, not narrower (decision 906).
  //
  // DIVERGENCE: the oracle returned these three in readdir order
  // (`["docs/b.md","docs/sub/c.md","docs/a.md"]`); this port sorts. The SET
  // is identical and the count — the only thing the `too_broad` cutoff and
  // the `written` count read — is unchanged.
  REQUIRE(paths.size() == 3);
  CHECK(paths == std::vector<std::string>{"docs/a.md", "docs/b.md", "docs/sub/c.md"});
}

TEST_CASE("a trailing slash is optional on a directory token", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const with    = classify(tree.root_, "docs/");
  auto const without = classify(tree.root_, "docs");
  CHECK(with.first == ti::classification::directory);
  CHECK(without.first == ti::classification::directory);
  CHECK(with.second == without.second);
}

TEST_CASE("an ambiguous basename yields EVERY match, never one guess", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const [cls, paths] = classify(tree.root_, "task.zig");
  CHECK(cls == ti::classification::basename);
  // Both, not the first found. Decision 906's over-declaration bias: a guess
  // here would silently under-declare, which is the direction that costs
  // correctness rather than throughput.
  REQUIRE(paths.size() == 2);
  CHECK(paths == std::vector<std::string>{"a/deep/task.zig", "b/task.zig"});
}

TEST_CASE("the basename fallback matches a filename exactly, not by suffix", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  // `b/task.zig` exists, so a bare `task.zig` finds it (asserted above). A
  // token that CARRIES a separator and misses stays `unresolved` — it does
  // NOT come back as `basename` proposing `b/task.zig`, which is what a
  // suffix or contains match would do.
  //
  // Note what this does NOT pin. `classify_token` guards the fallback on the
  // token being separator-free, and a break-probe replacing that guard with
  // `true` SURVIVED: the guard is an optimisation, since the search compares
  // against a directory entry's own filename and no filename contains `/`.
  // The rule with behaviour behind it is that the match runs against the
  // entry's FILENAME rather than its repo-relative path — mutating
  // `keep(name)` to `keep(child)` kills this case. Naming the guard here
  // would have claimed coverage this assertion cannot give.
  auto const [cls, paths] = classify(tree.root_, "nope/task.zig");
  CHECK(cls == ti::classification::unresolved);
  CHECK(paths.empty());

  // The paired positive: the same filename DOES match when named bare, so
  // the negative above is a rejected match rather than an empty tree.
  auto const bare = classify(tree.root_, "task.zig");
  CHECK(bare.first == ti::classification::basename);
  CHECK(bare.second.size() == 2);
}

TEST_CASE("an unplaceable token is reported, not silently dropped", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const [cls, paths] = classify(tree.root_, "src/nope/missing.zig");
  CHECK(cls == ti::classification::unresolved);
  CHECK(paths.empty());
  // Not writable — preview surfaces it, apply skips it, under either policy.
  CHECK_FALSE(ti::is_writable(cls, false));
  CHECK_FALSE(ti::is_writable(cls, true));
}

TEST_CASE("hidden entries are never proposed", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const [cls, paths] = classify(tree.root_, "pkg");
  CHECK(cls == ti::classification::directory);
  // The PRESENCE half: `pkg/real.zig` is there, so the walk did run and the
  // absence of `pkg/.git/config` below is a skip rather than an empty walk.
  REQUIRE(paths.size() == 1);
  CHECK(paths[0] == "pkg/real.zig");
  CHECK(std::ranges::find(paths, "pkg/.git/config") == paths.end());
  // …and the file the walk skipped really is on disk.
  REQUIRE(std::filesystem::is_regular_file(tree.root_ / "pkg/.git/config"));
}

TEST_CASE("an EXISTING but empty directory is unresolved, not an empty directory", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  // Oracle-captured: `emptydir/` reports `"classification":"unresolved"`
  // with no paths. Not derivable from the directory arm — a zero-path
  // `directory` would have been the obvious reading, and it is wrong.
  REQUIRE(std::filesystem::is_directory(tree.root_ / "emptydir"));
  auto const [cls, paths] = classify(tree.root_, "emptydir/");
  CHECK(cls == ti::classification::unresolved);
  CHECK(paths.empty());
}

TEST_CASE("an over-cap directory is too_broad with NO paths", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  auto const [cls, paths] = classify(tree.root_, "big/");
  CHECK(cls == ti::classification::too_broad);
  // Empty, NOT a truncated list. That is what makes expansion order unable
  // to reach state: no partial walk result ever escapes.
  CHECK(paths.empty());
  CHECK_FALSE(ti::is_writable(cls, false));
  CHECK_FALSE(ti::is_writable(cls, true));
}

TEST_CASE("the too_broad boundary is at exactly k_max_directory_expansion", "[engine][planning][touchinfer]") {
  // The cap test is `> limit`, so `limit` files is still a `directory` and
  // `limit + 1` is `too_broad`. Pinned because the oracle's walk overshoots
  // by one before stopping, and a tidier guard would move the boundary.
  scratch_tree tree;
  for (std::size_t i = 0; i < ti::k_max_directory_expansion; ++i) {
    put(tree.root_, std::format("atcap/f{}.txt", i));
  }
  for (std::size_t i = 0; i < ti::k_max_directory_expansion + 1; ++i) {
    put(tree.root_, std::format("overcap/f{}.txt", i));
  }
  REQUIRE(std::distance(std::filesystem::directory_iterator(tree.root_ / "atcap"), std::filesystem::directory_iterator{}) ==
          static_cast<std::ptrdiff_t>(ti::k_max_directory_expansion));

  auto const at   = classify(tree.root_, "atcap/");
  auto const over = classify(tree.root_, "overcap/");
  CHECK(at.first == ti::classification::directory);
  CHECK(at.second.size() == ti::k_max_directory_expansion);
  CHECK(over.first == ti::classification::too_broad);
  CHECK(over.second.empty());
}

// =========================================================================
// Whole-task inference
// =========================================================================

TEST_CASE("infer_from_text dedupes a token repeated across fields", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  const auto cands = ti::infer_from_text({.title       = "Fix src/engine/a.zig",
                                          .body        = "The bug is in src/engine/a.zig somewhere.",
                                          .next_action = "Edit src/engine/a.zig"},
                                         7, tree.root_);

  REQUIRE(cands.size() == 1);
  // Attributed to the FIRST field that produced it.
  CHECK(cands[0].evidence_ == ti::evidence::title);
  CHECK(cands[0].classification_ == ti::classification::resolved);
  CHECK(cands[0].repo_id == 7);
}

TEST_CASE("infer_from_text visits fields in title, body, next_action order", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  const auto cands =
      ti::infer_from_text({.title = "b/task.zig", .body = "docs/a.md", .next_action = "src/engine/a.zig"}, 1, tree.root_);
  REQUIRE(cands.size() == 3);
  CHECK(cands[0].token == "b/task.zig");
  CHECK(cands[0].evidence_ == ti::evidence::title);
  CHECK(cands[1].token == "docs/a.md");
  CHECK(cands[1].evidence_ == ti::evidence::body);
  CHECK(cands[2].token == "src/engine/a.zig");
  CHECK(cands[2].evidence_ == ti::evidence::next_action);
}

TEST_CASE("infer_from_text carries unresolved tokens through for review", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  const auto cands = ti::infer_from_text({.body = "Touch src/engine/a.zig and also src/imaginary.zig here."}, 1, tree.root_);

  // Presence AND absence in one call: if the fixture had not materialised,
  // BOTH would be `unresolved` and the first CHECK would fail.
  REQUIRE(cands.size() == 2);
  CHECK(cands[0].classification_ == ti::classification::resolved);
  CHECK(cands[1].classification_ == ti::classification::unresolved);

  const ti::inference inf{.task_id = 1, .candidates = cands};
  CHECK(inf.writable_count(false) == 1);
  CHECK(inf.review_count(false) == 1);
}

TEST_CASE("writable counts PATHS while review counts CANDIDATES", "[engine][planning][touchinfer]") {
  scratch_tree tree;
  build_fixture(tree.root_);

  // One `directory` candidate over three files. The asymmetry is the
  // oracle's and is not incidental: the preview line reads
  // `proposed:<paths>  review:<candidates>`.
  const auto          cands = ti::infer_from_text({.title = "Sweep docs/"}, 1, tree.root_);
  const ti::inference inf{.task_id = 1, .candidates = cands};
  REQUIRE(inf.candidates.size() == 1);
  REQUIRE(inf.candidates[0].paths.size() == 3);

  CHECK(inf.writable_count(false) == 0);
  CHECK(inf.review_count(false) == 1);
  CHECK(inf.writable_count(true) == 3);
  CHECK(inf.review_count(true) == 0);
}

TEST_CASE("writable classifications are exactly those that produce rows", "[engine][planning][touchinfer]") {
  // Default policy: only an exact path match writes. Directory and basename
  // expansions are proposed and shown, but withheld — measured to REDUCE
  // parallel-eligibility, since a wide set intersects peers and rule 2 drops
  // both sides.
  CHECK(ti::is_writable(ti::classification::resolved, false));
  CHECK_FALSE(ti::is_writable(ti::classification::directory, false));
  CHECK_FALSE(ti::is_writable(ti::classification::basename, false));
  CHECK_FALSE(ti::is_writable(ti::classification::unresolved, false));
  CHECK_FALSE(ti::is_writable(ti::classification::too_broad, false));

  // Opt-in policy: the wide classifications become writable; the two that
  // resolve to nothing never do, under either policy.
  CHECK(ti::is_writable(ti::classification::resolved, true));
  CHECK(ti::is_writable(ti::classification::directory, true));
  CHECK(ti::is_writable(ti::classification::basename, true));
  CHECK_FALSE(ti::is_writable(ti::classification::unresolved, true));
  CHECK_FALSE(ti::is_writable(ti::classification::too_broad, true));
}

TEST_CASE("the wire tokens are the oracle's", "[engine][planning][touchinfer]") {
  CHECK(ti::to_text(ti::evidence::title) == "title");
  CHECK(ti::to_text(ti::evidence::body) == "body");
  CHECK(ti::to_text(ti::evidence::next_action) == "next_action");
  CHECK(ti::to_text(ti::evidence::citation) == "citation");

  CHECK(ti::to_text(ti::classification::resolved) == "resolved");
  CHECK(ti::to_text(ti::classification::directory) == "directory");
  CHECK(ti::to_text(ti::classification::basename) == "basename");
  CHECK(ti::to_text(ti::classification::unresolved) == "unresolved");
  CHECK(ti::to_text(ti::classification::too_broad) == "too_broad");
}

// =========================================================================
// Database read
// =========================================================================

namespace {

/// @brief A scratch database path, removed with its sidecars on destruction.
struct scratch_db_path {
  std::filesystem::path path_;
  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_touchinfer_db_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;
  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(path_.string() + suffix, ec);
    }
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

} // namespace

TEST_CASE("infer reads the task's three text fields, NULLs included", "[engine][planning][touchinfer]") {
  scratch_tree    tree;
  scratch_db_path scratch;
  build_fixture(tree.root_);
  auto conn = open_migrated(scratch);

  // `body` and `next_action` are left NULL. The query coalesces them to
  // `''`, and an empty field is skipped rather than tokenized — so this also
  // proves the NULL path does not throw or produce a spurious candidate.
  REQUIRE(conn.execute("insert into tasks (scope_kind, title, slug, status) "
                       "values ('global','Fix src/engine/a.zig','t-null','todo')")
              .has_value());

  auto inferred = ti::infer(conn, 1, 5, tree.root_);
  REQUIRE(inferred.has_value());
  CHECK(inferred->task_id == 1);
  REQUIRE(inferred->candidates.size() == 1);
  CHECK(inferred->candidates[0].token == "src/engine/a.zig");
  CHECK(inferred->candidates[0].classification_ == ti::classification::resolved);
  CHECK(inferred->candidates[0].repo_id == 5);
}

TEST_CASE("infer reports not_found for a task id that does not exist", "[engine][planning][touchinfer]") {
  scratch_tree    tree;
  scratch_db_path scratch;
  build_fixture(tree.root_);
  auto conn = open_migrated(scratch);

  // Paired with the present case above so this cannot pass because the
  // fixture or the migration silently failed.
  REQUIRE(conn.execute("insert into tasks (scope_kind, title, slug, status) values ('global','present','t-present','todo')")
              .has_value());
  REQUIRE(ti::infer(conn, 1, 1, tree.root_).has_value());

  auto missing = ti::infer(conn, 999, 1, tree.root_);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == ti::infer_error::not_found);
}
