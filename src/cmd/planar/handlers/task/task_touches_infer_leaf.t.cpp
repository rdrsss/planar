// @file task_touches_infer_leaf.t.cpp
// @brief In-process tests for `planar task touches infer <task-id> [--repo
// <slug>] [--apply] [--wide] [--json]` (plan 996, task 6330).
//
// The extraction and resolution RULES are pinned at the engine level in
// `src/lib/engine/planning/touchinfer.t.cpp`. This file covers what that one
// cannot reach: dispatch wiring, repo resolution (`--repo` and cwd-prefix),
// the exit-code mapping, the exact preview bytes, and the `--apply`
// transaction's effect on `entity_links` and `task_touch_paths`.
//
// ## WHY PREVIEW-BY-DEFAULT IS THE POINT
//
// Per decision 906 the two error directions are asymmetric: an over-declared
// touch set costs recoverable throughput, an under-declared one costs
// correctness at fan-in, and inference cannot tell which it produced. So the
// verb proposes and stops. Two mappings follow, and both are asserted
// TOGETHER because either alone is satisfied by an implementation that got
// the other backwards:
//
//   * Without `--apply`, a run that PROPOSES rows still writes NOTHING.
//   * With `--apply`, a run whose every candidate is held back ALSO writes
//     nothing — not even the coarse repo edge — while still reporting
//     `"applied":true`. That field mirrors the flag, not the effect.
//
// ## THE FIXTURE ASSERTS ITS OWN SHAPE FIRST
//
// `the fixture arena is seeded as described` runs before any comparison. A
// heuristic's whole output is derived data, so an arena whose tree never
// materialised answers `unresolved` to everything and every "this is not
// written" assertion passes while testing nothing. The shape test pins the
// tree AND the seeded task rows.
//
// ## ORACLE PROVENANCE
//
// Every expected byte was captured from `zig/zig-out/bin/planar` built at
// this cycle's base, against a scratch arena (`PLANAR_DB` under a temp root,
// never the operator's database), each invocation through a pipe on both
// streams with the exit code read OUTSIDE the pipe. Nine seeded tasks were
// captured — one per arm — in text, `--json` and `--json --wide` form, plus
// `--apply` idempotence and four error arms.
//
// ## THE ONE DELIBERATE DIVERGENCE
//
// Expansion ORDER inside a candidate's `paths` array. The oracle emits
// readdir order (`["docs/b.md","docs/sub/c.md","docs/a.md"]` — APFS hash
// order, not a rule); this port sorts. Called out at the two assertions that
// touch it. `task touches list`, which reads the written rows back, orders
// in SQL and is byte-identical.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
  std::filesystem::path                           repo;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_tinfleaf_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "repo", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "repo").string()}},
      .db_path = root / "planar.db",
      .repo    = root / "repo",
  };
}

/// @brief Dispatch one invocation with the cwd inside the registered repo.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.repo, fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Dispatch with an explicit cwd, for the repo-resolution arms.
auto dispatch_from(const fixture& fx, const std::filesystem::path& cwd, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), cwd, fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run raw SQL, asserting it succeeded.
///
/// Every seed step is guarded: a seed that fails silently produces an arena
/// that was never built, and this verb would report a clean empty answer
/// against it.
auto exec_sql(const fixture& fx, std::string_view sql) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto ok = conn->execute(sql);
  INFO("sql: " << sql);
  REQUIRE(ok.has_value());
}

auto scalar_sql(const fixture& fx, std::string_view sql) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

void put(const std::filesystem::path& root, std::string_view rel) {
  const auto p = root / std::filesystem::path(rel);
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << "x";
}

/// @brief Seed the arena the oracle capture ran against: a repo tree plus
/// nine tasks, one per classification arm.
auto seed(const fixture& fx) -> void {
  // `init` is run from a SIBLING directory, not from the repo tree. It
  // registers a project at whatever cwd it is given, and a second project
  // sharing `repo/`'s exact `root_path` would tie on prefix length with the
  // one seeded below — the first row read would win, and which row that is
  // is not something this verb's contract says anything about. Keeping the
  // roots disjoint keeps the cwd-prefix assertions about the rule under
  // test rather than about SQLite's row order.
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "elsewhere", ec);
  REQUIRE(dispatch_from(fx, fx.root / "elsewhere", {"init", "--allow-no-repo", "--name", "t6330"}).code == 0);

  put(fx.repo, "src/engine/a.zig");
  put(fx.repo, "docs/a.md");
  put(fx.repo, "docs/b.md");
  put(fx.repo, "docs/sub/c.md");
  put(fx.repo, "a/deep/task.zig");
  put(fx.repo, "b/task.zig");
  put(fx.repo, "pkg/.git/config");
  put(fx.repo, "pkg/real.zig");
  std::filesystem::create_directories(fx.repo / "emptydir");
  for (int i = 1; i <= 70; ++i) {
    put(fx.repo, std::format("big/f{}.txt", i));
  }

  // `init` already registered a project; this one takes an id that cannot
  // collide and a `root_path` pointing at the tree above.
  exec_sql(fx, std::format("insert into projects (id, slug, name, root_path) values (900, 't6330repo', 't6330repo', '{}')",
                           fx.repo.string()));
  exec_sql(fx, "insert into plans (id, scope_kind, scope_id, title, slug, status) "
               "values (1, 'global', null, 'Infer plan', 't6330-plan', 'active')");

  auto task = [&](int id, std::string_view slug, std::string_view title, std::string_view body, std::string_view next_action) {
    exec_sql(fx, std::format("insert into tasks (id, scope_kind, scope_id, plan_id, title, body, next_action, status, "
                             "priority, slug) values ({}, 'global', null, 1, '{}', {}, {}, 'todo', 100, '{}')",
                             id, title, body.empty() ? std::string("null") : std::format("'{}'", body),
                             next_action.empty() ? std::string("null") : std::format("'{}'", next_action), slug));
  };

  task(1, "t6330-t1", "Fix src/engine/a.zig", "The bug is in src/engine/a.zig and also src/imaginary.zig here.",
       "Edit src/engine/a.zig");
  task(2, "t6330-t2", "Sweep docs/", "Rework docs/ entirely.", "");
  task(3, "t6330-t3", "Touch task.zig", "The helper task.zig moved.", "");
  task(4, "t6330-t4", "Expand big/", "Everything under big/ changes.", "");
  task(5, "t6330-t5", "Pure prose no paths", "Run it. Then verify. No paths here!", "");
  task(6, "t6330-t6", "t6330 bare", "", "");
  task(7, "t6330-t7", "Rework pkg/", "Everything in pkg/ moves.", "");
  task(8, "t6330-t8", "Rework emptydir/", "Clear out emptydir/ now.", "");
  task(9, "t6330-t9", "Cite src/engine/a.zig:12-40",
       "See docs/a.md:339 and `docs/b.md`, plus https://example.com/a.zig and /etc/passwd and ../out.zig.", "");
}

} // namespace

// =========================================================================
// The arena's own shape — runs before anything is compared
// =========================================================================

TEST_CASE("the fixture arena is seeded as described", "[cmd][task][touches][infer][fixture]") {
  auto fx = make_fixture("shape");
  seed(fx);

  // The TREE. Without it every token below resolves to `unresolved` and
  // every "nothing was written" assertion passes for the wrong reason.
  for (auto const* rel : {"src/engine/a.zig", "docs/a.md", "docs/b.md", "docs/sub/c.md", "a/deep/task.zig", "b/task.zig",
                          "pkg/real.zig", "pkg/.git/config"}) {
    INFO("fixture file: " << rel);
    REQUIRE(std::filesystem::is_regular_file(fx.repo / rel));
  }
  REQUIRE(std::filesystem::is_directory(fx.repo / "emptydir"));
  REQUIRE(std::filesystem::is_empty(fx.repo / "emptydir"));
  REQUIRE(std::distance(std::filesystem::directory_iterator(fx.repo / "big"), std::filesystem::directory_iterator{}) == 70);

  // The ROWS. `body` and `next_action` are asserted for the two tasks whose
  // NULL-ness is itself under test.
  REQUIRE(scalar_sql(fx, "select count(*) from tasks where id between 1 and 9") == 9);
  REQUIRE(scalar_sql(fx, "select body is null from tasks where id = 6") == 1);
  REQUIRE(scalar_sql(fx, "select next_action is not null from tasks where id = 1") == 1);
  REQUIRE(scalar_sql(fx, "select root_path = '" + fx.repo.string() + "' from projects where id = 900") == 1);
  // EXACTLY one project claims the repo tree. Two would tie on prefix length
  // and the cwd-resolution assertions below would silently be measuring
  // SQLite's row order instead of the longest-prefix rule.
  REQUIRE(scalar_sql(fx, "select count(*) from projects where root_path = '" + fx.repo.string() + "'") == 1);

  // Nothing is declared yet, on either level. Every apply assertion below is
  // measured against this zero.
  REQUIRE(scalar_sql(fx, "select count(*) from task_touch_paths") == 0);
  REQUIRE(scalar_sql(fx, "select count(*) from entity_links where relationship = 'touches'") == 0);
}

// =========================================================================
// The verb is wired
// =========================================================================

TEST_CASE("a SIBLING directory sharing a name prefix does not resolve to the repo", "[cmd][task][touches][infer][6331]") {
  // TASK 6331. The cwd was matched against `projects.root_path` with a raw
  // string `starts_with` and no path-component boundary, so a project
  // registered at `<root>/repo` also matched a cwd of `<root>/repo2` -- a
  // DIFFERENT repository that merely shares a name prefix.
  //
  // The consequence was silent: `touches infer` resolved tokens against the
  // wrong checkout and reported success, attributing work done in one
  // repository to a differently-named sibling. Reproduced from the oracle
  // under D2 until decision 1067 ended that rule.
  auto fx = make_fixture("sibling6331");
  seed(fx);

  // A real sibling directory whose path begins with the registered root.
  auto const      sibling = std::filesystem::path{fx.repo.string() + "2"};
  std::error_code ec;
  std::filesystem::create_directories(sibling / "src" / "engine", ec);
  {
    std::ofstream out{sibling / "src" / "engine" / "a.zig"};
    out << "// sibling\n";
  }

  auto const got = dispatch_from(fx, sibling, {"task", "touches", "infer", "1"});

  // It must NOT silently resolve to `t6330repo`. The verb has no repo for
  // this cwd and says so.
  CHECK(got.code != 0);
  INFO("stderr: " << got.err);
  CHECK(got.err.contains("no repo matches the current directory"));
  CHECK_FALSE(got.out.contains("t6330repo"));
}

TEST_CASE("the registered root itself still resolves — the boundary is a component, not a ban",
          "[cmd][task][touches][infer][6331]") {
  // Non-vacuity for the case above. A boundary check that simply refused
  // every cwd would satisfy it; the repo root and a subdirectory of it must
  // still match.
  auto fx = make_fixture("boundary6331");
  seed(fx);

  auto const at_root = dispatch_from(fx, fx.repo, {"task", "touches", "infer", "1"});
  INFO("root stderr: " << at_root.err);
  CHECK(at_root.code == 0);

  auto const in_subdir = dispatch_from(fx, fx.repo / "src", {"task", "touches", "infer", "1"});
  INFO("subdir stderr: " << in_subdir.err);
  CHECK(in_subdir.code == 0);
}

TEST_CASE("task touches infer dispatches instead of refusing at exit 64", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("wired");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "1"});
  INFO("stdout: " << res.out << "\nstderr: " << res.err);
  CHECK(res.code == 0);
  CHECK_FALSE(res.err.contains("not implemented in this build"));
  CHECK(res.out.starts_with("task:1  repo:t6330repo"));
}

// =========================================================================
// Preview — per classification arm
// =========================================================================

TEST_CASE("a resolved token previews as writable and an unresolved one as review", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t1");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "1"});
  REQUIRE(res.code == 0);
  // Captured verbatim from the oracle.
  CHECK(res.out == "task:1  repo:t6330repo  proposed:1  review:1  preview (nothing written)\n"
                   "  + src/engine/a.zig\n"
                   "      [resolved via title: src/engine/a.zig]\n"
                   "\nnot written — review:\n"
                   "  ? src/imaginary.zig  [unresolved via body]\n"
                   "\napply with: planar task touches infer 1 --apply\n");
}

TEST_CASE("the JSON envelope carries token, evidence, classification and paths", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t1json");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "1", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.out == R"({"task_id":1,"repo_id":900,"repo_slug":"t6330repo","applied":false,"written":0,"review":1,)"
                   R"("candidates":[{"token":"src/engine/a.zig","evidence":"title","classification":"resolved",)"
                   R"("paths":["src/engine/a.zig"]},{"token":"src/imaginary.zig","evidence":"body",)"
                   R"("classification":"unresolved","paths":[]}]})"
                   "\n");
}

TEST_CASE("a directory candidate is withheld by default and released by --wide", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t2");
  seed(fx);

  auto const plain = dispatch(fx, {"task", "touches", "infer", "2"});
  REQUIRE(plain.code == 0);
  CHECK(plain.out == "task:2  repo:t6330repo  proposed:0  review:1  preview (nothing written)\n"
                     "\nnot written — review:\n"
                     "  ? docs/  [directory via title — would declare 3 path(s)]\n"
                     "\n  1 directory/basename candidate(s) withheld — add --wide to include them.\n"
                     "  Wide expansion measured NEGATIVE for eligibility: it intersects peers\n"
                     "  and rule 2 drops both, so it can remove tasks that were otherwise fine.\n");
  // No `apply with:` footer: there is nothing to apply.
  CHECK_FALSE(plain.out.contains("apply with:"));

  auto const wide = dispatch(fx, {"task", "touches", "infer", "2", "--wide"});
  REQUIRE(wide.code == 0);
  // DIVERGENCE: the oracle listed these in readdir order
  // (`docs/b.md`, `docs/sub/c.md`, `docs/a.md`); this port sorts. The set and
  // the `proposed:3` count are the oracle's.
  CHECK(wide.out == "task:2  repo:t6330repo  proposed:3  review:0  preview (nothing written)\n"
                    "  + docs/a.md\n"
                    "      [directory via title: docs/]\n"
                    "  + docs/b.md\n"
                    "      [directory via title: docs/]\n"
                    "  + docs/sub/c.md\n"
                    "      [directory via title: docs/]\n"
                    "\napply with: planar task touches infer 2 --apply\n");
}

TEST_CASE("an ambiguous basename proposes every match", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t3");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "3", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.out == R"({"task_id":3,"repo_id":900,"repo_slug":"t6330repo","applied":false,"written":0,"review":1,)"
                   R"("candidates":[{"token":"task.zig","evidence":"title","classification":"basename",)"
                   R"("paths":["a/deep/task.zig","b/task.zig"]}]})"
                   "\n");
}

TEST_CASE("an over-cap directory is too_broad and stays unwritable under --wide", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t4");
  seed(fx);

  auto const plain = dispatch(fx, {"task", "touches", "infer", "4", "--json"});
  REQUIRE(plain.code == 0);
  CHECK(plain.out == R"({"task_id":4,"repo_id":900,"repo_slug":"t6330repo","applied":false,"written":0,"review":1,)"
                     R"("candidates":[{"token":"big/","evidence":"title","classification":"too_broad","paths":[]}]})"
                     "\n");

  // `--wide` releases `directory` and `basename`. It does NOT release
  // `too_broad`: `review` stays 1. That is the whole difference between "too
  // wide to write by default" and "too wide to review", and asserting only
  // the plain arm would not tell them apart.
  auto const wide = dispatch(fx, {"task", "touches", "infer", "4", "--wide", "--json"});
  REQUIRE(wide.code == 0);
  CHECK(wide.out.contains(R"("written":0,"review":1)"));
  CHECK(wide.out.contains(R"("classification":"too_broad")"));

  // And the text arm prints NO withheld-candidates hint, because the
  // withheld candidate is not one `--wide` could release.
  auto const text = dispatch(fx, {"task", "touches", "infer", "4"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("  ? big/  [too_broad via title]\n"));
  CHECK_FALSE(text.out.contains("add --wide to include them"));
}

TEST_CASE("a task with no path-shaped token yields an empty candidate list", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t5");
  seed(fx);

  auto const prose = dispatch(fx, {"task", "touches", "infer", "5", "--json"});
  REQUIRE(prose.code == 0);
  CHECK(prose.out == R"({"task_id":5,"repo_id":900,"repo_slug":"t6330repo","applied":false,"written":0,"review":0,)"
                     R"("candidates":[]})"
                     "\n");

  // A NULL body and next_action reach the same place by a different route.
  auto const bare = dispatch(fx, {"task", "touches", "infer", "6", "--json"});
  REQUIRE(bare.code == 0);
  CHECK(bare.out.contains(R"("candidates":[])"));

  auto const text = dispatch(fx, {"task", "touches", "infer", "5"});
  REQUIRE(text.code == 0);
  // Exactly one line: no review block, no apply footer.
  CHECK(text.out == "task:5  repo:t6330repo  proposed:0  review:0  preview (nothing written)\n");
}

TEST_CASE("hidden entries never reach the preview", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t7");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "7", "--json"});
  REQUIRE(res.code == 0);
  // The PRESENCE half — `pkg/real.zig` is proposed, so the walk ran…
  CHECK(res.out.contains(R"("paths":["pkg/real.zig"])"));
  // …and the absence is therefore a skip, not an empty walk.
  CHECK_FALSE(res.out.contains(".git"));
  REQUIRE(std::filesystem::is_regular_file(fx.repo / "pkg/.git/config"));
}

TEST_CASE("an existing but empty directory reports unresolved", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t8");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "8", "--json"});
  REQUIRE(res.code == 0);
  // Oracle-captured. A zero-path `directory` would have been the obvious
  // reading and is wrong.
  CHECK(res.out.contains(R"({"token":"emptydir/","evidence":"title","classification":"unresolved","paths":[]})"));
}

TEST_CASE("citations resolve while URLs, absolute paths and traversal do not", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("t9");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "9", "--json"});
  REQUIRE(res.code == 0);
  // Three resolved: the title's `:12-40` citation, the body's `:339` one, and
  // a backticked path. The URL, the absolute path and the `../` traversal
  // produce NO candidate at all — asserted alongside the three that do, so an
  // arena that resolved nothing could not pass.
  CHECK(res.out == R"({"task_id":9,"repo_id":900,"repo_slug":"t6330repo","applied":false,"written":0,"review":0,)"
                   R"("candidates":[{"token":"src/engine/a.zig","evidence":"title","classification":"resolved",)"
                   R"("paths":["src/engine/a.zig"]},{"token":"docs/a.md","evidence":"body","classification":"resolved",)"
                   R"("paths":["docs/a.md"]},{"token":"docs/b.md","evidence":"body","classification":"resolved",)"
                   R"("paths":["docs/b.md"]}]})"
                   "\n");
  CHECK_FALSE(res.out.contains("example.com"));
  CHECK_FALSE(res.out.contains("passwd"));
  CHECK_FALSE(res.out.contains("out.zig"));
}

// =========================================================================
// Apply
// =========================================================================

TEST_CASE("preview writes nothing and --apply writes both levels", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("apply");
  seed(fx);

  // Preview first. It PROPOSES a row and must still write nothing — the two
  // halves belong in one test because "wrote nothing" alone is also true of
  // a verb that proposes nothing.
  auto const preview = dispatch(fx, {"task", "touches", "infer", "1"});
  REQUIRE(preview.code == 0);
  REQUIRE(preview.out.contains("proposed:1"));
  CHECK(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 1") == 0);
  CHECK(scalar_sql(fx, "select count(*) from entity_links where from_id = 1 and relationship = 'touches'") == 0);

  auto const applied = dispatch(fx, {"task", "touches", "infer", "1", "--apply"});
  REQUIRE(applied.code == 0);
  CHECK(applied.out.contains("  APPLIED\n"));
  // No `apply with:` footer once applied.
  CHECK_FALSE(applied.out.contains("apply with:"));

  // The path row AND the coarse repo edge that path implies.
  CHECK(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 1 and repo_id = 900 "
                       "and path = 'src/engine/a.zig'") == 1);
  CHECK(scalar_sql(fx, "select count(*) from entity_links where from_kind = 'task' and from_id = 1 "
                       "and to_kind = 'repo' and to_id = 900 and relationship = 'touches'") == 1);
  // The unresolved candidate stayed out.
  CHECK(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 1") == 1);
}

TEST_CASE("apply with every candidate withheld writes nothing at all", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("applynone");
  seed(fx);

  // Task 2's only candidate is a `directory`, held back without `--wide`.
  auto const res = dispatch(fx, {"task", "touches", "infer", "2", "--apply", "--json"});
  REQUIRE(res.code == 0);
  // `applied` mirrors the FLAG, not the effect — oracle-captured, and easy
  // to "fix" into reporting false.
  CHECK(res.out.contains(R"("applied":true,"written":0)"));

  // Not even the coarse repo edge. The guard is `writable_count > 0`, so the
  // whole apply block is skipped; a port that hoisted the edge write out of
  // the block would leave a `touches` row here and change eligibility.
  CHECK(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 2") == 0);
  CHECK(scalar_sql(fx, "select count(*) from entity_links where from_id = 2 and relationship = 'touches'") == 0);

  // With `--wide` the same task writes all three.
  auto const wide = dispatch(fx, {"task", "touches", "infer", "2", "--apply", "--wide", "--json"});
  REQUIRE(wide.code == 0);
  CHECK(wide.out.contains(R"("applied":true,"written":3)"));
  CHECK(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 2") == 3);
  CHECK(scalar_sql(fx, "select count(*) from entity_links where from_id = 2 and relationship = 'touches'") == 1);
}

TEST_CASE("apply is idempotent and keeps reporting the attempt count", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("idem");
  seed(fx);

  auto const first = dispatch(fx, {"task", "touches", "infer", "2", "--apply", "--wide", "--json"});
  REQUIRE(first.code == 0);
  REQUIRE(first.out.contains(R"("written":3)"));
  REQUIRE(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 2") == 3);

  auto const second = dispatch(fx, {"task", "touches", "infer", "2", "--apply", "--wide", "--json"});
  REQUIRE(second.code == 0);
  // `written` counts ATTEMPTS, not inserts: `add_touch_path` is
  // `insert or ignore`, so the row count is unchanged while the report
  // repeats 3. Oracle-captured; the alternative reading (0 on a re-run) is
  // the natural one and is wrong.
  CHECK(second.out.contains(R"("written":3)"));
  CHECK(scalar_sql(fx, "select count(*) from task_touch_paths where task_id = 2") == 3);
  CHECK(scalar_sql(fx, "select count(*) from entity_links where from_id = 2 and relationship = 'touches'") == 1);
}

TEST_CASE("the rows written are readable through task touches list", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("roundtrip");
  seed(fx);

  REQUIRE(dispatch(fx, {"task", "touches", "infer", "2", "--apply", "--wide"}).code == 0);
  auto const listed = dispatch(fx, {"task", "touches", "list", "2", "--json"});
  REQUIRE(listed.code == 0);
  // `list` orders in SQL (`order by p.slug, ttp.path`), so its output was
  // already sorted in the oracle and is byte-identical here — the expansion
  // -order divergence does not reach the persisted view.
  CHECK(listed.out == R"({"task_id":2,"repos":["t6330repo"],"paths":[{"repo":"t6330repo","path":"docs/a.md"},)"
                      R"({"repo":"t6330repo","path":"docs/b.md"},{"repo":"t6330repo","path":"docs/sub/c.md"}]})"
                      "\n");
}

// =========================================================================
// Repo resolution and error arms
// =========================================================================

TEST_CASE("the --repo flag names the checkout explicitly", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("repoflag");
  seed(fx);

  // From a cwd outside every registered root, `--repo` still resolves.
  auto const res = dispatch_from(fx, fx.root, {"task", "touches", "infer", "1", "--repo", "t6330repo", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains(R"("repo_slug":"t6330repo")"));
  CHECK(res.out.contains(R"("classification":"resolved")"));
}

TEST_CASE("the longest matching root_path prefix wins", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("longest");
  seed(fx);

  // A submodule checkout under the outer repo. The oracle picks it over its
  // superproject by prefix LENGTH, which is why this is not a plain lookup.
  std::filesystem::create_directories(fx.repo / "sub/src");
  put(fx.repo, "sub/src/only_here.zig");
  exec_sql(fx, std::format("insert into projects (id, slug, name, root_path) values (901, 'sub6330', 'sub6330', '{}')",
                           (fx.repo / "sub").string()));
  exec_sql(fx, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) "
               "values (10, 'global', null, 1, 'Edit src/only_here.zig', 'todo', 100, 't6330-t10')");

  // From inside the submodule: the inner repo, and the token resolves
  // relative to IT.
  auto const inner = dispatch_from(fx, fx.repo / "sub", {"task", "touches", "infer", "10", "--json"});
  REQUIRE(inner.code == 0);
  CHECK(inner.out.contains(R"("repo_slug":"sub6330")"));
  CHECK(inner.out.contains(R"("classification":"resolved")"));

  // From the outer repo the SAME task resolves to nothing, because
  // `src/only_here.zig` does not exist there. Pairing the two is what proves
  // the prefix choice actually changed the answer rather than the token
  // happening to resolve either way.
  auto const outer = dispatch_from(fx, fx.repo, {"task", "touches", "infer", "10", "--json"});
  REQUIRE(outer.code == 0);
  CHECK(outer.out.contains(R"("repo_slug":"t6330repo")"));
  CHECK(outer.out.contains(R"("classification":"unresolved")"));
}

TEST_CASE("a non-integer task id refuses at exit 2", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("badid");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "notanumber"});
  CHECK(res.code == 2);
  CHECK(res.err.contains("task id must be an integer, got 'notanumber'"));
}

TEST_CASE("a missing task refuses at exit 1", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("notask");
  seed(fx);

  // Paired with a present id so this cannot pass on an unseeded arena.
  REQUIRE(dispatch(fx, {"task", "touches", "infer", "1"}).code == 0);
  auto const res = dispatch(fx, {"task", "touches", "infer", "999"});
  CHECK(res.code == 1);
  CHECK(res.err.contains("task 999 not found"));
}

TEST_CASE("an unknown --repo slug refuses at exit 1", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("norepo");
  seed(fx);

  auto const res = dispatch(fx, {"task", "touches", "infer", "1", "--repo", "nosuchrepo"});
  CHECK(res.code == 1);
  CHECK(res.err.contains("repo 'nosuchrepo' not found"));
}

TEST_CASE("a cwd inside no registered checkout refuses with guidance", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("nocwd");
  seed(fx);

  auto const res = dispatch_from(fx, std::filesystem::temp_directory_path(), {"task", "touches", "infer", "1"});
  CHECK(res.code == 1);
  // The message names the recovery, which is the whole reason it is not a
  // bare "not found".
  CHECK(res.err.contains("no repo matches the current directory; pass --repo <slug>"));
}

TEST_CASE("a repo registered with an empty root_path refuses at exit 2", "[cmd][task][touches][infer]") {
  auto fx = make_fixture("noroot");
  seed(fx);

  exec_sql(fx, "insert into projects (id, slug, name, root_path) values (902, 'rootless6330', 'rootless6330', '')");
  auto const res = dispatch(fx, {"task", "touches", "infer", "1", "--repo", "rootless6330"});
  CHECK(res.code == 2);
  CHECK(res.err.contains("repo has no root_path recorded; cannot resolve paths against it"));
}
