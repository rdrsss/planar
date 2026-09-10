// @file drafting_leaves.t.cpp
// @brief Leaf-level tests for the TWENTY-FOUR `edit | view | diff | review`
// leaves across `plan`, `task`, `question`, `decision`, `scenario` and
// `artifact` (plan 996 roadmap M12; the link-anchored sixteen at task 6205,
// `plan` and `task` at task 6208).
//
// Same shape as `question_leaves.t.cpp` and its three siblings: dispatch the
// real tree and table against a scratch root, then assert the
// operator-visible output AND the durable state. Exit code 0 is not the
// contract.
//
// ## HOW THE `$EDITOR` SPAWN IS TESTED, decided before it was implemented
//
// This is the first place in the port that spawns an INTERACTIVE CHILD
// PROCESS, and the parity harness cannot diff an interactive editor. A
// quartet that is green because the test never launched an editor is the
// exact silent-degradation shape this milestone keeps finding, so the design
// is written down here rather than left implicit in the assertions.
//
// The mechanism is a STUB EDITOR: a shell script written into the fixture
// root, pointed at by `PLANAR_EDITOR` in the fixture's environment map.
// `context::env()` is what `editor.cppm` resolves through — see that
// module's header for why it does not call `std::getenv` — so this needs no
// process-environment mutation and no `putenv` race between Catch2 cases.
//
// THREE INDEPENDENT WITNESSES, each of which fails on its own if the spawn
// stops happening:
//
//   1. THE ARGV WITNESS. The stub appends its own `$1` to a witness file.
//      A non-empty witness proves a child ran; the recorded path proves it
//      was handed the TEMP file (`$TMPDIR/planar-edit-<hex>.md`) and NOT the
//      workbench file, which is a real distinction — the Zig original edits
//      a temp copy and writes back, and a port that handed over the
//      workbench path directly would pass every content assertion while
//      changing what a concurrent `workbench push` observes mid-edit.
//
//   2. THE PROVENANCE WITNESS, and the load-bearing one. The stub rewrites
//      the front matter's `title:` to a sentinel that appears NOWHERE in
//      `src/`. The sentinel can only reach the `questions` row by way of a
//      spawned process writing a file this binary then read back. If the
//      spawn silently stopped, `edit` would find its own rendered content
//      unchanged and report `aborted: no changes made` — and the DB title
//      assertion fails. That is why the test asserts the ROW, not just that
//      `edit` exited 0.
//
//   3. THE EXIT-CODE WITNESS. A second stub mutates the file and then
//      `exit 3`. The row must be UNCHANGED and stderr must carry `aborted:
//      editor exited with code 3`. A port that ignored the child's status
//      would apply the mutation and pass witnesses 1 and 2 while breaking
//      the cancel contract every editor-driven verb depends on.
//
// BREAK-PROBE, run and recorded rather than asserted to be sufficient: with
// `spawn_inherit`'s `fork`/`execv` pair replaced by an immediate
// `return 0` — the child never created, everything else untouched — the
// cases below fail as:
//
//   - "the $EDITOR spawn is real: the stub's argv is witnessed"
//       witness file absent          -> FAILED (witness 1)
//   - "an $EDITOR that rewrites the title lands that title in the row"
//       err == "aborted: no changes made\n", title still "Q linked"
//                                    -> FAILED (witness 2)
//   - "a non-zero $EDITOR exit aborts without writing"
//       err empty, title == "SHOULD-NOT-LAND"
//                                    -> FAILED (witness 3)
//
// All three go red, independently. `scripts/break-probe.sh` carries the
// probe.
//
// ## Every expectation here came from RUNNING the oracle
//
// Captured against `zig/zig-out/bin/planar` in a pinned scratch arena
// (`PLANAR_DB`, `PLANAR_HOME`, `PLANAR_CONFIG_PATH`, `PLANAR_LOCAL_HOME`,
// `PLANAR_WORKBENCH_ROOT` and `HOME` all redirected).
//
// ONE CAPTURE HAZARD COST A ROUND HERE AND IS WORTH THE PARAGRAPH. The first
// oracle run redirected stderr with `2> file`, and the unlinked-`view` cases
// came back as a bare `error: NoPlanLink` plus a stack trace with NO prose
// line — from which the obvious conclusion is that `view` has no prose arm.
// It does. `../parity_harness.hpp` documents why: the Zig writer uses
// POSITIONAL writes, so a second write from one process to one SEEKABLE file
// starts at offset 0 and eats the first. `view` writes stderr twice. Piping
// each stream through `cat` instead recovers both lines. Every stderr
// expectation below was re-captured through a pipe.
//
// ## The findings, in the order they surprised
//
//   - THE FOUR FAMILIES AGREE. The standing note predicted a fifth
//     per-family divergence — `scenario` working where `decision` does not,
//     because `scenario add --plan` writes `from_kind = 'test_scenario'`.
//     All four families write a `derives-from` edge; `scenario` alone
//     spells its `from_kind` `test_scenario` and `entity_link_kind` already
//     maps it. The observed split was a LINKED row against an UNLINKED one.
//     Pinned below by running all four through the same cases.
//   - `view` and `diff` USE DIFFERENT RENDERERS, so `view` then `diff`
//     always reports a diff. The canonical renderer carries `**Created:**`
//     and `**Updated:**`; the thin one does not.
//   - ...EXCEPT for `artifact`, where the two renderers agree byte for byte
//     and it is the PATH that differs instead. So all four are noisy after
//     a `view` and only three are noisy for the documented reason. Found by
//     writing the loop as one `contains("**Created:**")` check and watching
//     `artifact` fail it, which is the whole argument for looping over the
//     four rather than checking one and generalising.
//   - `diff 0` refuses at exit 2; `view 0` does not.
//   - `review` with no verdict is `diff`, byte for byte.
//   - `question view 999` reports `NoPlanLink`, NOT `no question with id
//     999` — the resolver queries `entity_links` before the entity exists.
//   - "title and status ONLY" passes through THREE gates, not zero: the
//     front-matter parser's per-kind status set, `scenario`'s transition
//     matrix (that family alone), and the table's own CHECK constraints.
//     `question -> answered` clears the first two and is refused by the
//     third as a bare `error: QueryFailed`, because the reduced flow never
//     writes the `answer_body`/`answered_at` the constraint requires.
//     Oracle-confirmed and reproduced rather than improved.
//
// ## `plan` and `task` (task 6208), and why they were a SECOND oracle run
//
// Task 6205 wired sixteen leaves and left these eight declared-but-unported
// on purpose, with a `dispatch.t.cpp` block that would fail the day someone
// wired them without redoing the run. That block did its job. These two
// families forward into the SAME module — the four handler bodies below are
// reused unchanged — but they arrive through a different anchor resolver
// and a different path builder, and the run found a real divergence the
// "same module, therefore done" argument would have shipped wrong:
//
//   - A NONEXISTENT id is `not_found` for them and `no_plan_link` for the
//     other four, because `plan` walks `plans.parent_plan_id` and `task`
//     reads `tasks.plan_id` — neither touches `entity_links`. So
//     `plan diff 999` says `no plan with id 999` where `question diff 999`
//     says `question 999 is not linked to a plan`. Both exit 1, so the
//     exit code does NOT separate them and only the prose does. This is
//     the arm `editflow.cpp` documents as unreachable; it is unreachable
//     for four of six families and the only reachable arm for the other
//     two.
//
// Three behaviours the hold-note named were confirmed rather than changed:
//
//   - `walk_to_anchor` climbs the WHOLE chain. On plans 1 <- 2 <- 3 all
//     three render `anchor_plan_id: 1`, as does a task pinned to plan 3. A
//     resolver that returned `parent_plan_id` directly would still resolve
//     every path and would silently split the feature directory in two.
//   - The ANCHOR plan is the feature's `README.md`; any other plan is
//     `plans/<slug>.md`.
//   - `task_workbench_dir` tries repo-scope, then `touches`, then `cross`.
//     All three arms are pinned, AND the precedence case the chain's shape
//     alone does not settle: a task scoped `repo:proj` that ALSO touches
//     `proj2` renders into `tasks/proj/`. Repo-scope wins.
//
// And two generalisations that were checked instead of assumed:
//
//   - Unlike `artifact`, `view`/`edit` and `diff`/`review` AGREE about the
//     path for both families, so their `diff` reports a content diff rather
//     than a whole-document deletion hunk.
//   - The thin-vs-canonical renderer split DOES still apply, so `plan view`
//     followed by `plan diff` is noisy for the documented reason.
//   - `apply_mutations` guards `scenario` alone, so a `task` status edit
//     moves `todo -> done` skipping `doing`, and back. Run in both
//     directions rather than inferred from the guard's shape. Gate 1 still
//     bites: an unknown status is refused by the front-matter parser.
//
// BREAK-PROBES for these cases, run via `scripts/break-probe.sh`. Each
// mutant built and was KILLED by the single named test:
//
//   editflow: drop the anchor-plan README branch
//       -> "plan view renders the ANCHOR to README.md ..."          killed
//   editflow: walk_to_anchor returns the first parent, no loop
//       -> "walk_to_anchor climbs the whole chain, not one level"   killed
//   editflow: disable the repo-scope arm so touches/cross win
//       -> "task_workbench_dir tries repo-scope, then touches ..."  killed
//   editflow: unify not_found onto the no_plan_link wording
//       -> "a nonexistent plan or task is not_found, ..."           killed
//   editflow: extend the scenario-only transition guard to task
//       -> "a task status edit runs NO transition guard, ..."       killed
//   editor:   remove the fork so no child is ever created
//       -> "the $EDITOR spawn is real for plan and task too, ..."   killed
//   dispatch: unwire `task review` from the table
//       -> "every leaf is in exactly one of the two populations"    killed

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

/// @brief One dispatched invocation's observable result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
///
/// `PLANAR_WORKBENCH_ROOT` and `TMPDIR` are BOTH redirected into the root:
/// the first so rendered workbench files land in the arena, the second so
/// the editor's temp file does too. `PATH` is carried through from the real
/// environment because the pager chain and the stub editor are real
/// programs that have to be found — it is the one variable here that is not
/// scratch, and it is read-only.
///
/// Nothing reads the operator's `~/.planar`.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_draft_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "wb", ec);
  std::filesystem::create_directories(root / "tmp", ec);

  auto const* path = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe)
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()},
                  {"PLANAR_WORKBENCH_ROOT", (root / "wb").string()},
                  {"TMPDIR", (root / "tmp").string()},
                  {"PATH", path != nullptr ? std::string{path} : std::string{"/usr/bin:/bin"}}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table inside `fx`.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Open the fixture's database directly, for row assertions.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief One scalar from a read-only query.
/// @param conn An open connection.
/// @param sql The query; a test-local literal, never operator input.
/// @return The first column of the first row, or the empty string.
auto scalar(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  if (*stepped == planar::db::step_result::done) {
    return {};
  }
  return stmt->column_text(0);
}

/// @brief Read a whole file, or the empty string when absent.
/// @param path The file.
/// @return The bytes.
auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

/// @brief Write an executable shell script.
/// @param path Where to write it.
/// @param body The script body, `#!` line included.
void write_script(const std::filesystem::path& path, std::string_view body) {
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << body;
  }
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
  REQUIRE(!ec);
}

/// @brief Install a stub PAGER that records its argv instead of paging.
///
/// `view` execs a pager that INHERITS stdout, so the pager's output does
/// not reach `ctx.out()` and cannot be asserted from a dispatched
/// invocation. Redirecting it to a witness file is what makes the spawn
/// observable at all — and, incidentally, keeps the real `cat` from
/// spraying workbench documents through the ctest log.
/// @param fx The fixture.
/// @return The witness file path.
auto install_stub_pager(fixture& fx) -> std::filesystem::path {
  auto const witness = fx.root / "pager-witness";
  auto const script  = fx.root / "stub-pager";
  write_script(script, std::format("#!/bin/sh\nprintf '%s\\n' \"$1\" >> {}\n", (fx.root / "pager-witness").string()));
  fx.vars["PAGER"] = script.string();
  return witness;
}

/// @brief Seed an anchor plan plus one plan-LINKED row in every family.
///
/// Every row is id 1 in its own table and every one carries a
/// `derives-from` edge to plan 1, so all four families are reachable
/// through the identical argv shape.
/// @param fx The fixture.
void seed_linked(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--slug", "anchor", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "Q linked", "--body", "qb", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "D linked", "--body", "db", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"scenario", "add", "S linked", "--body", "sb", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "A linked", "--kind", "adr", "--body", "ab", "--plan", "1", "--json"}).code == 0);
}

/// @brief The four families, with the slug their seeded title renders to.
struct family {
  std::string_view name;
  std::string_view slug;
};

/// @brief The four link-anchored families task 6205 wired.
/// @return The families.
auto families() -> std::vector<family> {
  return {
      {"question", "1-q-linked.md"}, {"decision", "1-d-linked.md"}, {"scenario", "1-s-linked.md"}, {"artifact", "1-a-linked.md"}};
}

/// @brief The feature directory `seed_hierarchy` renders into.
///
/// The association level is PRESENT here where `seed_linked`'s cases have
/// none, because `plan` and `task` need registered projects — `repo:` scope
/// and `touches` both name one — and registering a project means creating
/// the association that owns it. `project:proj` sanitises to `project_proj`.
/// @param fx The fixture.
/// @return The absolute feature directory.
auto hierarchy_feature_dir(const fixture& fx) -> std::filesystem::path {
  return fx.root / "wb" / "project_proj" / "p1-anchor";
}

/// @brief Seed the plan CHAIN and the task SHAPES that `plan` and `task`
/// reach and the four link-anchored families never do.
///
/// EVERY ROW HERE EXISTS TO PIN ONE ORACLE-DERIVED BEHAVIOUR, so nothing in
/// it is incidental:
///
///   plans   1 <- 2 <- 3. Plan 1 is the anchor (`parent_plan_id` null), so
///           `walk_to_anchor` is exercised at depth 0, 1 and 2. The four
///           link-anchored families only ever reach depth 0.
///
///   task 1  association-scoped, `touches proj2`     -> `tasks/proj2/`
///   task 2  association-scoped, NO touches          -> `tasks/cross/`
///   task 3  `repo:proj`-scoped                      -> `tasks/proj/`
///   task 4  `repo:proj2`-scoped, on the GRANDCHILD  -> `tasks/proj2/`
///   task 5  `repo:proj`-scoped AND `touches proj2`  -> `tasks/proj/`
///
///   Task 5 is the one the chain's SHAPE does not settle: reading
///   `task_workbench_dir` tells you repo-scope is tried first, but not
///   whether the oracle agrees. It does — repo-scope wins over `touches`,
///   captured by running it.
///
/// @param fx The fixture.
void seed_hierarchy(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj2").string()}).code == 0);

  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--slug", "anchor", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Child", "--slug", "child", "--parent", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Grand", "--slug", "grand", "--parent", "2", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "add", "T touches", "--plan", "2", "--scope", "project:proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T cross", "--plan", "3", "--scope", "project:proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T repo", "--plan", "2", "--scope", "repo:proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T deep", "--plan", "3", "--scope", "repo:proj2", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T both", "--plan", "2", "--scope", "repo:proj", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "proj2"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "5", "proj2"}).code == 0);
}

} // namespace

// ===========================================================================
// The premise: all four families write the edge the resolver queries
// ===========================================================================

TEST_CASE("all four families write a derives-from edge, and scenario alone spells it test_scenario", "[cmd][drafting][anchor]") {
  // THE FINDING THIS TASK'S BRIEF PREDICTED WRONG, pinned as a row
  // assertion so it cannot be re-argued from behaviour. The standing note
  // held that `scenario` resolves an anchor where its siblings do not,
  // because `scenario add --plan` writes `from_kind = 'test_scenario'` and
  // the others write something the resolver misses. What the resolver
  // queries is `entity_link_kind(kind)`, which maps `scenario` to
  // `test_scenario` and every other kind to itself — so all four match.
  //
  // Asserted on the TABLE rather than by running the quartet, because the
  // quartet passing is the consequence and this is the cause.
  auto const fx = make_fixture("edges");
  seed_linked(fx);

  auto        conn = open_db(fx);
  std::string edges;
  auto        stmt = conn.prepare("select from_kind, from_id, to_kind, to_id, relationship from entity_links order by from_kind");
  REQUIRE(stmt.has_value());
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!edges.empty()) {
      edges += ';';
    }
    edges += std::format("{}|{}|{}|{}|{}", stmt->column_text(0), stmt->column_int64(1), stmt->column_text(2),
                         stmt->column_int64(3), stmt->column_text(4));
  }
  CHECK(edges == "artifact|1|plan|1|derives-from;decision|1|plan|1|derives-from;"
                 "question|1|plan|1|derives-from;test_scenario|1|plan|1|derives-from");
}

// ===========================================================================
// view
// ===========================================================================

TEST_CASE("view renders the entity to its workbench file and spawns a pager", "[cmd][drafting][view]") {
  auto       fx      = make_fixture("view");
  auto const witness = install_stub_pager(fx);
  seed_linked(fx);

  // All four families, identical argv shape, identical outcome. The loop
  // IS the assertion that they agree — running one and asserting the other
  // three by inspection is what produced the wrong standing note.
  for (auto const& fam : families()) {
    INFO("family=" << fam.name);
    auto const res = dispatch(fx, {std::string{fam.name}, "view", "1"});
    CHECK(res.code == 0);
    CHECK(res.err.empty());
    // NOTHING on stdout: the pager inherits the real stdout descriptor, so
    // the document reaches the terminal from a CHILD process and never
    // passes through `ctx.out()`. A port that printed the file itself
    // would produce output here — and would then double-print under a real
    // `$PAGER`.
    CHECK(res.out.empty());
  }

  // The pager was spawned once per family, and handed the workbench path.
  auto const paged = read_file(witness);
  auto const dir   = fx.root / "wb" / "p1-anchor";
  CHECK(paged == std::format("{}\n{}\n{}\n{}\n", (dir / "questions" / "1-q-linked.md").string(),
                             (dir / "decisions" / "1-d-linked.md").string(), (dir / "scenarios" / "1-s-linked.md").string(),
                             // `artifact` alone lands under `artifacts/`.
                             (dir / "artifacts" / "1-a-linked.md").string()));

  // And the file it was handed holds the THIN render: front matter, a
  // heading, a status line — and NO `**Created:**`/`**Updated:**` pair.
  // That absence is the whole reason `diff` disagrees with `view`, so it
  // is asserted as bytes rather than described.
  CHECK(read_file(dir / "questions" / "1-q-linked.md") == "---\n"
                                                          "entity_kind: question\n"
                                                          "entity_id: 1\n"
                                                          "anchor_plan_id: 1\n"
                                                          "title: Q linked\n"
                                                          "status: open\n"
                                                          "---\n"
                                                          "\n"
                                                          "# Question 1: Q linked\n"
                                                          "\n"
                                                          "**Status:** open\n"
                                                          "\n"
                                                          "qb\n");
}

TEST_CASE("view on an entity with no plan edge writes nothing and names the resolver", "[cmd][drafting][view]") {
  auto       fx      = make_fixture("viewunlinked");
  auto const witness = install_stub_pager(fx);
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "Q", "--body", "b", "--json"}).code == 0);

  auto const res = dispatch(fx, {"question", "view", "1"});
  CHECK(res.code == 1);
  CHECK(res.out.empty());
  // Two lines. The oracle emits both and then SEVEN stack frames naming
  // absolute paths inside its own build tree; this build stops after the
  // tag. THE ONE DELIBERATE DIVERGENCE — see `editflow.cppm`'s header.
  CHECK(res.err == "error: cannot resolve anchor plan for question 1: NoPlanLink\nerror: NoPlanLink\n");

  // The resolver runs FIRST, so nothing was written and no pager ran. This
  // is the assertion that the refusal is not a half-completed verb.
  CHECK(read_file(witness).empty());
  CHECK(!std::filesystem::exists(fx.root / "wb" / "p1-anchor"));
}

TEST_CASE("view reports NoPlanLink for an id that does not exist at all", "[cmd][drafting][view]") {
  // NOT `no question with id 999`. The anchor resolver queries
  // `entity_links` before anything queries `questions`, so a nonexistent id
  // is indistinguishable from an unlinked one. Oracle-captured, and the
  // reason the `not_found` arm in `editflow`'s prose mapping is unreachable
  // for these four families.
  auto fx = make_fixture("viewmissing");
  static_cast<void>(install_stub_pager(fx));
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  auto const res = dispatch(fx, {"question", "view", "999"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: cannot resolve anchor plan for question 999: NoPlanLink\nerror: NoPlanLink\n");
}

// ===========================================================================
// diff
// ===========================================================================

TEST_CASE("diff against an absent workbench file renders the whole document as a deletion", "[cmd][drafting][diff]") {
  auto const fx = make_fixture("diffempty");
  seed_linked(fx);

  // Nothing has written the workbench tree yet, so the filesystem side is
  // empty and every line of the CANONICAL render is a deletion.
  auto const res = dispatch(fx, {"question", "diff", "1"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());
  CHECK(res.out.starts_with(std::format("--- db:question:1\n+++ fs:{}\n@@ -1,",
                                        (fx.root / "wb" / "p1-anchor" / "questions" / "1-q-linked.md").string())));
  // The canonical renderer's timestamps are PRESENT here and absent from
  // `view`'s output above. Asserted by content rather than by line count so
  // it fails if the two renderers are ever unified.
  CHECK(res.out.contains("-**Created:**"));
  CHECK(res.out.contains("-**Updated:**"));

  // ...and the hunk is a pure deletion. Checked on the HEADER, because the
  // body cannot be checked for "no + lines": the `+++ fs:` header itself
  // starts with `+`, so a naive scan for `+` matches it and passes
  // vacuously. The `+0,0` half of the range is the unambiguous statement
  // that the filesystem side contributed nothing.
  auto const hunk = res.out.find("@@ -1,");
  REQUIRE(hunk != std::string::npos);
  CHECK(res.out.substr(hunk).starts_with(std::format("@@ -1,{} +0,0 @@\n", std::ranges::count(res.out, '\n') - 3)));
}

TEST_CASE("view then diff reports a diff, because the two use different renderers", "[cmd][drafting][diff]") {
  // THE MOST SURPRISING BEHAVIOUR IN THE QUARTET, and the one most likely
  // to be "fixed" by a later reader who assumes it is a bug. `view` writes
  // the THIN render; `diff` compares the CANONICAL render against that same
  // file. So a `view` immediately followed by a `diff` is never silent.
  //
  // Pinned on all four families, because "they agree" is this cycle's
  // finding and a per-family exception here would be the counterexample.
  auto fx = make_fixture("viewthendiff");
  static_cast<void>(install_stub_pager(fx));
  seed_linked(fx);

  for (auto const& fam : families()) {
    INFO("family=" << fam.name);
    REQUIRE(dispatch(fx, {std::string{fam.name}, "view", "1"}).code == 0);
    auto const res = dispatch(fx, {std::string{fam.name}, "diff", "1"});
    CHECK(res.code == 0);
    CHECK(!res.out.empty());
  }

  // ...but NOT for the same reason in all four, and this is the one place
  // the families genuinely part company in this flow. It was found by
  // writing the loop above as a single `contains("**Created:**")` check and
  // watching `artifact` fail it.
  //
  //   question / decision / scenario
  //       The CONTENT differs. `view` wrote the thin render to the path
  //       `diff` reads, and `diff` reports the timestamp lines the
  //       canonical render adds -- a MODIFICATION hunk.
  //   artifact
  //       The content is IDENTICAL. The canonical artifact renderer emits
  //       the same body the thin one does, byte for byte -- no `**Created:**`
  //       pair at all. What differs is the PATH: `view` wrote under
  //       `artifacts/` and `diff` reads the feature root, which is empty,
  //       so the hunk is a pure DELETION of a document that exists on disk
  //       three directories away.
  //
  // The consequence is that unifying the artifact paths would make
  // `artifact diff` silent after a `view` while leaving its three siblings
  // noisy -- so the path split is load-bearing in a way a reader looking
  // only at the renderers would not predict.
  for (auto const& fam : {"question", "decision", "scenario"}) {
    INFO("family=" << fam);
    auto const res = dispatch(fx, {std::string{fam}, "diff", "1"});
    CHECK(res.out.contains("-**Created:**"));
    CHECK(res.out.contains(" # ")); // a context line: this is a MODIFICATION.
  }
  {
    auto const res = dispatch(fx, {"artifact", "diff", "1"});
    CHECK(!res.out.contains("**Created:**"));
    CHECK(res.out.contains("@@ -1,")); // ...and a pure DELETION.
    CHECK(res.out.contains("+0,0 @@"));
  }
}

TEST_CASE("diff is silent when the workbench file already matches the database", "[cmd][drafting][diff]") {
  // The `diff -u` contract: equal content writes NOTHING and exits 0. A
  // caller cannot tell "no changes" from "did not run" by stdout, which is
  // why the equality is established by `workbench push` -- the CANONICAL
  // writer -- rather than by this test reconstructing the bytes.
  //
  // Reconstructing them was tried first, by stripping the `-` prefix off a
  // prior `diff`'s deletion hunk, and it did not produce byte-equal content.
  // Using `workbench push` instead is both simpler and a STRONGER
  // assertion: it proves `diff` and `workbench push` agree on the path AND
  // on the bytes, which is exactly the agreement the artifact family
  // famously does not have.
  auto const fx = make_fixture("diffsilent");
  seed_linked(fx);

  auto const noisy = dispatch(fx, {"question", "diff", "1"});
  REQUIRE(noisy.code == 0);
  REQUIRE(!noisy.out.empty()); // ...so silence below is a change, not the default.

  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  auto const quiet = dispatch(fx, {"question", "diff", "1"});
  CHECK(quiet.code == 0);
  CHECK(quiet.out.empty());
  CHECK(quiet.err.empty());

  // ...and `review` in preview mode is silent on the same input, since it
  // is `diff`.
  auto const reviewed = dispatch(fx, {"question", "review", "1"});
  CHECK(reviewed.code == 0);
  CHECK(reviewed.out.empty());

  // ...while `review --json` still reports, with `has_changes:false`. The
  // JSON envelope is emitted unconditionally where the text form is not,
  // and that asymmetry is what a scripted caller depends on.
  auto const json = dispatch(fx, {"question", "review", "1", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.contains(R"("has_changes":false)"));
}

TEST_CASE("diff refuses a non-positive id where view does not", "[cmd][drafting][diff]") {
  // The verb asymmetry, pinned in the one place it is visible. `diff` and
  // `review` check `id <= 0`; `view` and `edit` do not and fall through to
  // the resolver. Both shapes are the oracle's.
  auto fx = make_fixture("diffzero");
  static_cast<void>(install_stub_pager(fx));
  seed_linked(fx);

  for (auto const& fam : families()) {
    INFO("family=" << fam.name);
    auto const refused = dispatch(fx, {std::string{fam.name}, "diff", "0"});
    CHECK(refused.code == 2);
    CHECK(refused.err == std::format("error: {} id must be a positive integer, got 0\n", fam.name));

    // The SAME id through `view` reaches the resolver instead.
    auto const through = dispatch(fx, {std::string{fam.name}, "view", "0"});
    CHECK(through.code == 1);
    CHECK(through.err == std::format("error: cannot resolve anchor plan for {} 0: NoPlanLink\nerror: NoPlanLink\n", fam.name));
  }

  // ...and a non-integer refuses the same way on both.
  auto const bad = dispatch(fx, {"question", "view", "notanint"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: question id must be an integer, got 'notanint'\n");
}

TEST_CASE("artifact view and artifact diff disagree about where the file lives", "[cmd][drafting][diff][artifact]") {
  // THE ONE GENUINE PER-FAMILY DIVERGENCE, inherited from the artifact
  // cycle and preserved rather than unified. `view` writes under
  // `artifacts/`; `diff` reads the feature ROOT. So a `view` does not make
  // a subsequent `diff` any quieter — it leaves a file in a place `diff`
  // never looks, and `workbench push` later reports as `new_on_fs` drift.
  //
  // Unifying the two paths is a tempting one-line change and would alter
  // what `workbench push` reports, which is why the divergence is asserted
  // as a pair of `exists` checks rather than described in a comment alone.
  auto fx = make_fixture("artifactpath");
  static_cast<void>(install_stub_pager(fx));
  seed_linked(fx);

  REQUIRE(dispatch(fx, {"artifact", "view", "1"}).code == 0);

  auto const dir = fx.root / "wb" / "p1-anchor";
  CHECK(std::filesystem::exists(dir / "artifacts" / "1-a-linked.md"));
  CHECK(!std::filesystem::exists(dir / "1-a-linked.md"));

  // ...and `diff` names the ROOT path, and still reports the whole document
  // as a deletion because the root file does not exist.
  auto const res = dispatch(fx, {"artifact", "diff", "1"});
  CHECK(res.code == 0);
  CHECK(res.out.contains(std::format("+++ fs:{}\n", (dir / "1-a-linked.md").string())));
  CHECK(res.out.contains("@@ -1,"));

  // The three siblings do NOT have this split: `view` writes exactly where
  // `diff` reads. Asserted so the artifact case reads as the exception it
  // is rather than as the rule.
  for (auto const& fam : {std::pair{"question", "questions"}, {"decision", "decisions"}, {"scenario", "scenarios"}}) {
    INFO("family=" << fam.first);
    REQUIRE(dispatch(fx, {std::string{fam.first}, "view", "1"}).code == 0);
    auto const sibling = dispatch(fx, {std::string{fam.first}, "diff", "1"});
    CHECK(sibling.out.contains(std::format("+++ fs:{}", (dir / fam.second).string())));
  }
}

// ===========================================================================
// review
// ===========================================================================

TEST_CASE("review with no verdict is diff, byte for byte", "[cmd][drafting][review]") {
  auto const fx = make_fixture("reviewpreview");
  seed_linked(fx);

  for (auto const& fam : families()) {
    INFO("family=" << fam.name);
    auto const differ = dispatch(fx, {std::string{fam.name}, "diff", "1"});
    auto const viewer = dispatch(fx, {std::string{fam.name}, "review", "1"});
    CHECK(viewer.code == differ.code);
    CHECK(viewer.out == differ.out);
    CHECK(viewer.err == differ.err);
    CHECK(!differ.out.empty()); // ...and neither was trivially empty.
  }
}

TEST_CASE("review --json emits the summary envelope with an explicit null verdict", "[cmd][drafting][review]") {
  auto const fx = make_fixture("reviewjson");
  seed_linked(fx);

  auto const path = (fx.root / "wb" / "p1-anchor" / "questions" / "1-q-linked.md").string();

  // `verdict` is an explicit `null` in preview mode, NOT an omitted key.
  // A reflected `std::optional` would have dropped it; the envelope is
  // hand-composed for exactly this reason, and the key ORDER is contract.
  auto const preview = dispatch(fx, {"question", "review", "1", "--json"});
  CHECK(preview.code == 0);
  CHECK(preview.err.empty());
  CHECK(preview.out == std::format(R"({{"entity":"question","id":1,"anchor_plan_id":1,"workbench_path":"{}",)"
                                   R"("verdict":null,"has_changes":true,"persisted":false,"persistence":"none"}})"
                                   "\n",
                                   path));

  auto const approved = dispatch(fx, {"question", "review", "1", "--approve", "--json"});
  CHECK(approved.code == 0);
  CHECK(approved.out == std::format(R"({{"entity":"question","id":1,"anchor_plan_id":1,"workbench_path":"{}",)"
                                    R"("verdict":"approve","has_changes":true,"persisted":false,"persistence":"none"}})"
                                    "\n",
                                    path));

  // `request-changes` renders with the DASH, not the underscore its enum
  // arm carries.
  auto const changes = dispatch(fx, {"question", "review", "1", "--request-changes", "--json"});
  CHECK(changes.code == 0);
  CHECK(changes.out.contains(R"("verdict":"request-changes")"));

  // NOTHING is persisted, and `persisted:false` is not merely a claim —
  // there is no per-entity review table, so the assertion is that the row
  // and the audit trail are untouched by all three invocations.
  auto conn = open_db(fx);
  CHECK(scalar(conn, "select status from questions where id = 1") == "open");
  CHECK(scalar(conn, "select count(*) from audit_log where entity_kind = 'question' and verb = 'review'") == "0");
}

TEST_CASE("review in text form with a verdict reports the summary", "[cmd][drafting][review]") {
  auto const fx = make_fixture("reviewtext");
  seed_linked(fx);

  auto const path = (fx.root / "wb" / "p1-anchor" / "decisions" / "1-d-linked.md").string();
  auto const res  = dispatch(fx, {"decision", "review", "1", "--approve"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());
  CHECK(res.out == std::format("decision 1 review: approve (persisted: no; reason: no per-entity review table)\n"
                               "workbench: {}\nanchor_plan: 1\nchanges_pending: yes\n",
                               path));
}

TEST_CASE("review refuses both verdicts together, before opening the database", "[cmd][drafting][review]") {
  auto const fx = make_fixture("reviewboth");
  seed_linked(fx);

  for (auto const& fam : families()) {
    INFO("family=" << fam.name);
    auto const res = dispatch(fx, {std::string{fam.name}, "review", "1", "--approve", "--request-changes"});
    CHECK(res.code == 2);
    CHECK(res.out.empty());
    CHECK(res.err == "error: --approve and --request-changes are mutually exclusive\n");
  }
}

// ===========================================================================
// edit — and the $EDITOR spawn
// ===========================================================================

TEST_CASE("the $EDITOR spawn is real: the stub's argv is witnessed", "[cmd][drafting][edit][spawn]") {
  // WITNESS 1. See this file's header for the three-witness design and the
  // break-probe that puts each of them red.
  auto fx = make_fixture("editargv");
  seed_linked(fx);

  auto const witness = fx.root / "editor-witness";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\nprintf '%s\\n' \"$1\" >> {}\n", witness.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 0);
  // The stub changed nothing, so the flow reports the no-change abort.
  CHECK(res.err == "aborted: no changes made\n");

  auto const recorded = read_file(witness);
  REQUIRE(!recorded.empty()); // <-- the spawn happened at all.

  // ...and it was handed the TEMP file, not the workbench file. The Zig
  // original edits a temp copy and writes back; a port that passed the
  // workbench path straight to the editor would satisfy every content
  // assertion in this file while changing what a concurrent `workbench
  // push` sees mid-edit.
  std::string_view const path{recorded.data(), recorded.size() - 1};
  CHECK(path.starts_with((fx.root / "tmp" / "planar-edit-").string()));
  CHECK(path.ends_with(".md"));
  CHECK(!path.contains("/wb/"));
}

TEST_CASE("an $EDITOR that rewrites the title lands that title in the row", "[cmd][drafting][edit][spawn]") {
  // WITNESS 2, THE LOAD-BEARING ONE. `EDITED-BY-STUB-7f3a` appears nowhere
  // in `src/`. It can only reach the `questions` row by way of a spawned
  // process writing a file this binary then read back. If the spawn
  // silently stopped, `edit` would find its own render unchanged and report
  // `aborted: no changes made`, and this case fails on both the stderr and
  // the row.
  auto fx = make_fixture("editapply");
  seed_linked(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^title: .*/title: EDITED-BY-STUB-7f3a/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 0);
  CHECK(res.out.empty());
  CHECK(res.err.empty()); // A clean title-only edit is SILENT.

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from questions where id = 1") == "EDITED-BY-STUB-7f3a");
  // `updated_at` moved with it — the UPDATE sets it unconditionally.
  CHECK(scalar(conn, "select case when updated_at > created_at then 'moved' else 'stale' end from questions where id = 1") ==
        "moved");
}

TEST_CASE("a non-zero $EDITOR exit aborts without writing", "[cmd][drafting][edit][spawn]") {
  // WITNESS 3. The stub mutates the file AND exits 3. A port that ignored
  // the child's status would apply the mutation, pass witnesses 1 and 2,
  // and break the cancel contract every editor-driven verb depends on.
  auto fx = make_fixture("editabort");
  seed_linked(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^title: .*/title: SHOULD-NOT-LAND/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n"
                                        "exit 3\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 0); // An abort is not a failure — the oracle exits 0.
  CHECK(res.out.empty());
  CHECK(res.err == "aborted: editor exited with code 3\n");

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from questions where id = 1") == "Q linked");
}

TEST_CASE("edit applies title and status together", "[cmd][drafting][edit]") {
  auto fx = make_fixture("editstatus");
  seed_linked(fx);

  // `open -> wontfix`. Both halves land in ONE update.
  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^status: .*/status: wontfix/' -e 's/^title: .*/title: T2/' \"$1\" "
                                        "> \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from questions where id = 1") == "T2");
  CHECK(scalar(conn, "select status from questions where id = 1") == "wontfix");
}

TEST_CASE("edit refuses a status the FRONT-MATTER PARSER does not know", "[cmd][drafting][edit]") {
  // The reduced flow does NOT accept arbitrary status text. Before
  // `editflow` sees it, `engine.workbench.parse` validates `status:` against
  // the per-kind set, and the refusal names the parse error rather than the
  // status.
  //
  // Worth pinning because the natural reading of "title and status
  // mutations ONLY" is that any status string is written straight through.
  // It is not: there are THREE gates, and they refuse in three different
  // wordings -- see the CHECK-constraint case below for the third.
  auto fx = make_fixture("editbadstatus");
  seed_linked(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^status: .*/status: nonsense-status/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: failed to parse editor output for question 1: InvalidFieldValue\n"
                   "error: ParseFailed\n");

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select status from questions where id = 1") == "open");
}

TEST_CASE("edit surfaces a schema CHECK constraint as a bare QueryFailed", "[cmd][drafting][edit]") {
  // THE THIRD GATE, and the sharpest edge in the reduced flow.
  //
  // `answered` is a VALID question status: the front-matter parser accepts
  // it and there is no transition guard on this family. But the `questions`
  // table carries
  //
  //     check ((status = 'answered' and answer_body is not null
  //             and answered_at is not null) or (status != 'answered'))
  //
  // and `editflow` writes title and status ONLY -- it never touches
  // `answer_body` or `answered_at`. So a front-matter edit that would move a
  // question to `answered` is refused by SQLite, and the operator gets a
  // bare `error: QueryFailed` with no mention of the constraint, the
  // column, or the fact that `question answer` is the verb that works.
  //
  // ORACLE-CONFIRMED, byte for byte on the first line -- the oracle emits
  // the same `error: QueryFailed` and then its stack trace through
  // `applyMutations`. This is reproduced rather than improved: a friendlier
  // message here would be a behaviour change wearing a bug fix's clothes,
  // and the parity lane would flag it. It is the kind of thing that
  // deserves its own task.
  auto fx = make_fixture("editcheck");
  seed_linked(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^status: .*/status: answered/' -e 's/^title: .*/title: T2/' \"$1\" "
                                        "> \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 1);
  CHECK(res.out.empty());
  CHECK(res.err == "error: QueryFailed\n");

  // The update is ATOMIC in the useful sense: the title did not land
  // either, because title and status go in one statement.
  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from questions where id = 1") == "Q linked");
  CHECK(scalar(conn, "select status from questions where id = 1") == "open");
}

TEST_CASE("edit warns before overwriting a drifted workbench file", "[cmd][drafting][edit]") {
  // The database is authoritative in the reduced flow: a workbench file
  // that has drifted is OVERWRITTEN before the editor opens. The operator's
  // only notice is this line, so it is pinned verbatim.
  auto fx = make_fixture("editdrift");
  seed_linked(fx);

  auto const path = fx.root / "wb" / "p1-anchor" / "questions" / "1-q-linked.md";
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << "locally edited, and about to be discarded\n";
  }

  write_script(fx.root / "stub-editor", "#!/bin/sh\nexit 0\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 0);
  CHECK(res.err == std::format("warning: overwriting local workbench file with DB-rendered content before edit: {}\n"
                               "aborted: no changes made\n",
                               path.string()));
  // ...and the drift really is gone.
  CHECK(read_file(path).contains("# Question 1: Q linked"));
}

TEST_CASE("edit on an entity with no plan edge never spawns an editor", "[cmd][drafting][edit][spawn]") {
  // The resolver runs BEFORE the editor. A port that rendered first and
  // resolved later would open an editor on a document it cannot save,
  // which is worse than the refusal.
  auto fx = make_fixture("editunlinked");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "Q", "--body", "b", "--json"}).code == 0);

  auto const witness = fx.root / "editor-witness";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\nprintf '%s\\n' \"$1\" >> {}\n", witness.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"question", "edit", "1"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: cannot resolve anchor plan for question 1: NoPlanLink\nerror: NoPlanLink\n");
  CHECK(read_file(witness).empty());
}

TEST_CASE("all four families run the same edit flow", "[cmd][drafting][edit][spawn]") {
  // The families-agree finding, applied to the one verb where a per-family
  // difference would be most expensive to discover late.
  auto fx = make_fixture("editall");
  seed_linked(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^title: .*/title: RETITLED/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  for (auto const& fam : families()) {
    INFO("family=" << fam.name);
    auto const res = dispatch(fx, {std::string{fam.name}, "edit", "1"});
    CHECK(res.code == 0);
    CHECK(res.err.empty());
  }

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from questions where id = 1") == "RETITLED");
  CHECK(scalar(conn, "select title from decisions where id = 1") == "RETITLED");
  CHECK(scalar(conn, "select title from test_scenarios where id = 1") == "RETITLED");
  CHECK(scalar(conn, "select title from artifacts where id = 1") == "RETITLED");
}

TEST_CASE("scenario alone guards its status transition through the editor path", "[cmd][drafting][edit][scenario]") {
  // THE ONE PLACE THE FOUR FAMILIES REALLY DO DIVERGE IN THIS FLOW, and it
  // is in `apply_mutations` rather than in any handler. `scenario` runs the
  // status matrix; the other three accept whatever the front matter says.
  //
  // The asymmetry is the Zig original's and is deliberate there:
  // `engine.planning.scenario` owns `verify`/`retire`, and editflow is the
  // escape hatch for a direct front-matter edit, so the guard is applied on
  // the escape hatch alone.
  auto fx = make_fixture("editscenario");
  seed_linked(fx);

  // `verified` PARSES for a scenario -- it is in the kind's status set --
  // and is an ILLEGAL move from `draft`, whose only legal targets are
  // `ready` and `retired`. That combination is what reaches the guard; a
  // status the parser rejects never gets that far, which is what the first
  // attempt at this case discovered.
  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^status: .*/status: verified/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const guarded = dispatch(fx, {"scenario", "edit", "1"});
  CHECK(guarded.code == 1);
  CHECK(guarded.err == "error: IllegalTransition\n");

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select status from test_scenarios where id = 1") == "draft");

  // ...and the LEGAL move through the same path succeeds, so the guard is
  // a guard and not a blanket refusal of every status edit.
  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^status: .*/status: ready/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  auto const allowed = dispatch(fx, {"scenario", "edit", "1"});
  CHECK(allowed.code == 0);
  CHECK(allowed.err.empty());

  auto after = open_db(fx);
  CHECK(scalar(after, "select status from test_scenarios where id = 1") == "ready");
}

TEST_CASE("the other three families run NO transition guard on the editor path", "[cmd][drafting][edit]") {
  // The other half of the asymmetry, and the half that is easy to assert by
  // accident from the scenario case alone. `decision` moving `proposed ->
  // withdrawn` would be legal anyway; the interesting probe is a move the
  // family's own dedicated verb would refuse.
  //
  // `decision`'s matrix has no `proposed -> superseded` edge (`decision
  // supersede` requires an accepted decision), and the editor path takes it
  // regardless -- because `apply_mutations` consults the matrix for
  // `scenario` ONLY.
  auto fx = make_fixture("editnoguard");
  seed_linked(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^status: .*/status: superseded/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"decision", "edit", "1"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select status from decisions where id = 1") == "superseded");
}

// ===========================================================================
// `plan` and `task` — the eight leaves task 6205 held for their own oracle
// run, wired at task 6208.
//
// The cases below are the run. Each pins one thing the four link-anchored
// families do NOT exercise, so none of them can be satisfied by the fact
// that the shared `editflow` module already compiled for these two kinds —
// which is precisely the claim 6205 declined to ship on.
// ===========================================================================

TEST_CASE("plan view renders the ANCHOR to README.md and every other plan under plans/", "[cmd][drafting][view][plan]") {
  // ORACLE PATH 1 of 3. `entity_rel_path`'s `plan` arm branches on
  // `id == anchor_id`, which the four link-anchored families can never
  // reach: none of them IS a plan. A port that dropped the branch would put
  // the anchor at `plans/anchor.md`, still exit 0, still render correct
  // bytes, and silently stop being the file `workbench pull` writes.
  auto fx = make_fixture("planviewpaths");
  seed_hierarchy(fx);
  install_stub_pager(fx);

  auto const dir = hierarchy_feature_dir(fx);

  REQUIRE(dispatch(fx, {"plan", "view", "1"}).code == 0);
  CHECK(std::filesystem::exists(dir / "README.md"));
  CHECK(!std::filesystem::exists(dir / "plans" / "anchor.md"));

  REQUIRE(dispatch(fx, {"plan", "view", "2"}).code == 0);
  CHECK(std::filesystem::exists(dir / "plans" / "child.md"));

  REQUIRE(dispatch(fx, {"plan", "view", "3"}).code == 0);
  CHECK(std::filesystem::exists(dir / "plans" / "grand.md"));
}

TEST_CASE("walk_to_anchor climbs the whole chain, not one level", "[cmd][drafting][view][plan][anchor]") {
  // ORACLE PATH 2 of 3, and the one a one-level fixture cannot catch. With
  // plans 1 <- 2 <- 3, a resolver that returned `parent_plan_id` directly
  // instead of looping would give plan 3 an anchor of 2. Every path would
  // still resolve, every file would still be written, and the feature
  // directory would silently split in two.
  //
  // The anchor is asserted through the RENDERED front matter rather than
  // the resolver, because that is where an operator sees it.
  auto fx = make_fixture("planwalk");
  seed_hierarchy(fx);
  install_stub_pager(fx);

  auto const dir = hierarchy_feature_dir(fx);

  for (auto const& [id, file] :
       std::vector<std::pair<std::string, std::string>>{{"1", "README.md"}, {"2", "plans/child.md"}, {"3", "plans/grand.md"}}) {
    INFO("plan " << id << " at depth " << id);
    REQUIRE(dispatch(fx, {"plan", "view", id}).code == 0);
    auto const body = read_file(dir / file);
    REQUIRE(!body.empty());
    CHECK(body.contains("anchor_plan_id: 1\n"));
  }

  // A task pinned to the GRANDCHILD walks the same chain, through
  // `tasks.plan_id` first. Task 4 is on plan 3.
  REQUIRE(dispatch(fx, {"task", "view", "4"}).code == 0);
  auto const deep = read_file(dir / "tasks" / "proj2" / "4-t-deep.md");
  REQUIRE(!deep.empty());
  CHECK(deep.contains("anchor_plan_id: 1\n"));
}

TEST_CASE("task_workbench_dir tries repo-scope, then touches, then cross", "[cmd][drafting][view][task]") {
  // ORACLE PATH 3 of 3, all four arms plus the precedence case. See
  // `seed_hierarchy` for why each task exists.
  //
  // The precedence case (task 5) is the one that had to be RUN: reading the
  // function tells you repo-scope is checked first, but "the code I am
  // porting does X" is not evidence that the oracle does X — it is evidence
  // about the same code. Task 5 is scoped `repo:proj` AND touches `proj2`,
  // and lands under `tasks/proj/`.
  auto fx = make_fixture("taskdirs");
  seed_hierarchy(fx);
  install_stub_pager(fx);

  auto const dir = hierarchy_feature_dir(fx);

  struct expectation {
    std::string id;
    std::string rel;
    std::string why;
  };
  for (auto const& [id, rel, why] :
       std::vector<expectation>{{"1", "tasks/proj2/1-t-touches.md", "association-scoped, resolved by its touches edge"},
                                {"2", "tasks/cross/2-t-cross.md", "association-scoped with no touches, the cross fallback"},
                                {"3", "tasks/proj/3-t-repo.md", "repo-scoped"},
                                {"4", "tasks/proj2/4-t-deep.md", "repo-scoped to the OTHER repo, on the grandchild plan"},
                                {"5", "tasks/proj/5-t-both.md", "repo-scoped AND touching: repo-scope wins"}}) {
    INFO("task " << id << ": " << why);
    REQUIRE(dispatch(fx, {"task", "view", id}).code == 0);
    CHECK(std::filesystem::exists(dir / rel));
  }

  // Discrimination: the two arms are not collapsing onto one directory.
  // Without this, a `task_workbench_dir` that returned the scope slug for
  // everything would pass three of the five rows above.
  CHECK(std::filesystem::exists(dir / "tasks" / "cross"));
  CHECK(std::filesystem::exists(dir / "tasks" / "proj"));
  CHECK(std::filesystem::exists(dir / "tasks" / "proj2"));
  CHECK(!std::filesystem::exists(dir / "tasks" / "proj" / "1-t-touches.md"));
  CHECK(!std::filesystem::exists(dir / "tasks" / "proj2" / "5-t-both.md"));
}

TEST_CASE("plan and task diff read the SAME path view wrote, unlike artifact", "[cmd][drafting][diff][plan][task]") {
  // The generalisation this file's header warns against, checked instead of
  // assumed. `artifact view` and `artifact diff` disagree about where the
  // file lives; `plan` and `task` do NOT, so their `diff` finds the file
  // and reports a CONTENT diff rather than a whole-document deletion hunk.
  //
  // The tell is the hunk header: an absent file gives `@@ -1,N +0,0 @@`.
  auto fx = make_fixture("plantaskpath");
  seed_hierarchy(fx);
  install_stub_pager(fx);

  REQUIRE(dispatch(fx, {"plan", "view", "2"}).code == 0);
  auto const plan_diff = dispatch(fx, {"plan", "diff", "2"});
  CHECK(plan_diff.code == 0);
  CHECK(plan_diff.out.contains("plans/child.md"));
  CHECK(!plan_diff.out.contains("+0,0"));

  REQUIRE(dispatch(fx, {"task", "view", "3"}).code == 0);
  auto const task_diff = dispatch(fx, {"task", "diff", "3"});
  CHECK(task_diff.code == 0);
  CHECK(task_diff.out.contains("tasks/proj/3-t-repo.md"));
  CHECK(!task_diff.out.contains("+0,0"));

  // ...and the THIN-vs-CANONICAL renderer split still applies to them, so
  // the diff is non-empty for the documented reason rather than because the
  // file is missing. Both timestamp lines are what the canonical renderer
  // adds and the thin one omits.
  CHECK(plan_diff.out.contains("-**Created:**"));
  CHECK(plan_diff.out.contains("-**Updated:**"));
  CHECK(task_diff.out.contains("-**Created:**"));
  CHECK(task_diff.out.contains("-**Updated:**"));
}

TEST_CASE("a nonexistent plan or task is not_found, where the other four are no_plan_link",
          "[cmd][drafting][diff][plan][task][divergence]") {
  // THE FINDING THAT EARNED THIS CYCLE. `plan` walks `plans.parent_plan_id`
  // and `task` reads `tasks.plan_id`; neither consults `entity_links`, so
  // neither can report `NoPlanLink` for a row that is simply absent.
  //
  // `prose_error`'s `not_found` arm is documented in `editflow.cpp` as
  // unreachable — true for the four families 6205 wired, false for these
  // two, where it is the ONLY arm reachable. Both spellings exit 1, so the
  // exit code does not separate them and only the prose does.
  auto fx = make_fixture("plantasknotfound");
  seed_hierarchy(fx);

  auto const plan_diff = dispatch(fx, {"plan", "diff", "999"});
  CHECK(plan_diff.code == 1);
  CHECK(plan_diff.err == "error: no plan with id 999\n");

  auto const task_diff = dispatch(fx, {"task", "diff", "999"});
  CHECK(task_diff.code == 1);
  CHECK(task_diff.err == "error: no task with id 999\n");

  // `review` shares `load_snapshot`, so it shares the prose.
  CHECK(dispatch(fx, {"plan", "review", "999"}).err == "error: no plan with id 999\n");
  CHECK(dispatch(fx, {"task", "review", "999"}).err == "error: no task with id 999\n");

  // THE DISCRIMINATION, and the reason this case is not just three string
  // literals: a link-anchored family on the SAME database and the same
  // missing-id shape reports the other wording entirely. If the two arms
  // were ever unified, this half goes red.
  auto const question_diff = dispatch(fx, {"question", "diff", "999"});
  CHECK(question_diff.code == 1);
  CHECK(question_diff.err == "error: question 999 is not linked to a plan; cannot resolve anchor plan\n");

  // `view` takes the bare-tag path for all six, but the TAG differs for the
  // same reason. (The oracle follows this with stack frames; see this
  // module's header for the one deliberate divergence.)
  auto const plan_view = dispatch(fx, {"plan", "view", "999"});
  CHECK(plan_view.code == 1);
  CHECK(plan_view.err == "error: cannot resolve anchor plan for plan 999: NotFound\nerror: NotFound\n");
  auto const task_view = dispatch(fx, {"task", "view", "999"});
  CHECK(task_view.code == 1);
  CHECK(task_view.err == "error: cannot resolve anchor plan for task 999: NotFound\nerror: NotFound\n");
}

TEST_CASE("plan and task keep the diff-strict / view-lenient id asymmetry", "[cmd][drafting][diff][plan][task]") {
  // The asymmetry `drafting.cppm`'s table documents, confirmed on the two
  // families that reach it through a different resolver. `view 0` is NOT
  // refused by the handler; it reaches the resolver and reports what the
  // resolver finds — which for these two is `NotFound`, not `NoPlanLink`.
  auto fx = make_fixture("plantaskid");
  seed_hierarchy(fx);

  auto const plan_zero = dispatch(fx, {"plan", "diff", "0"});
  CHECK(plan_zero.code == 2);
  CHECK(plan_zero.err == "error: plan id must be a positive integer, got 0\n");

  auto const task_zero = dispatch(fx, {"task", "diff", "0"});
  CHECK(task_zero.code == 2);
  CHECK(task_zero.err == "error: task id must be a positive integer, got 0\n");

  auto const lenient = dispatch(fx, {"plan", "view", "0"});
  CHECK(lenient.code == 1); // reached the resolver, not the id guard
  CHECK(lenient.err.contains("cannot resolve anchor plan for plan 0: NotFound"));

  // Non-integer is refused by the shared parse for both verbs.
  CHECK(dispatch(fx, {"plan", "view", "abc"}).err == "error: plan id must be an integer, got 'abc'\n");
  CHECK(dispatch(fx, {"task", "diff", "abc"}).err == "error: task id must be an integer, got 'abc'\n");
}

TEST_CASE("plan and task review emit the envelope with their own entity name", "[cmd][drafting][review][plan][task]") {
  auto fx = make_fixture("plantaskreview");
  seed_hierarchy(fx);
  install_stub_pager(fx);

  auto const dir = hierarchy_feature_dir(fx);

  auto const plan_json = dispatch(fx, {"plan", "review", "2", "--json"});
  CHECK(plan_json.code == 0);
  CHECK(plan_json.out == std::format(R"({{"entity":"plan","id":2,"anchor_plan_id":1,"workbench_path":"{}","verdict":null,)"
                                     R"("has_changes":true,"persisted":false,"persistence":"none"}})"
                                     "\n",
                                     (dir / "plans" / "child.md").string()));

  auto const task_json = dispatch(fx, {"task", "review", "3", "--json"});
  CHECK(task_json.code == 0);
  CHECK(task_json.out == std::format(R"({{"entity":"task","id":3,"anchor_plan_id":1,"workbench_path":"{}","verdict":null,)"
                                     R"("has_changes":true,"persisted":false,"persistence":"none"}})"
                                     "\n",
                                     (dir / "tasks" / "proj" / "3-t-repo.md").string()));

  // The text form with a verdict names the family and reports the same
  // anchor the walk resolved.
  auto const verdict = dispatch(fx, {"task", "review", "3", "--approve"});
  CHECK(verdict.code == 0);
  CHECK(verdict.out.starts_with("task 3 review: approve (persisted: no; reason: no per-entity review table)\n"));
  CHECK(verdict.out.contains("anchor_plan: 1\n"));
  CHECK(verdict.out.contains("changes_pending: yes\n"));

  // Both verdicts together is refused before the database opens, same as
  // the other four.
  auto const both = dispatch(fx, {"plan", "review", "2", "--approve", "--request-changes"});
  CHECK(both.code == 2);
  CHECK(both.err == "error: --approve and --request-changes are mutually exclusive\n");
}

TEST_CASE("the $EDITOR spawn is real for plan and task too, and gets the TEMP path", "[cmd][drafting][edit][spawn][plan][task]") {
  // WITNESSES 1 AND 2 on the two families this task wired. These leaves
  // reach the editor through the same shared body, but they reach it AFTER
  // a different anchor resolver and a different path builder, so a
  // regression in either would surface here and nowhere in the sixteen
  // cases above.
  //
  // The sentinels `PLAN-EDITED-BY-STUB-4c19` and `TASK-EDITED-BY-STUB-4c19`
  // appear nowhere in `src/`. Neither can reach a row except by way of a
  // spawned process writing a file this binary then read back.
  auto fx = make_fixture("plantaskedit");
  seed_hierarchy(fx);

  auto const witness = fx.root / "editor-witness";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\n"
                                                    "printf '%s\\n' \"$1\" >> {}\n"
                                                    "sed -e \"s/^title: .*/title: EDITED-BY-STUB-4c19/\" \"$1\" > \"$1.new\"\n"
                                                    "mv \"$1.new\" \"$1\"\n",
                                                    witness.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const plan_res = dispatch(fx, {"plan", "edit", "2"});
  CHECK(plan_res.code == 0);
  CHECK(plan_res.err.empty()); // a clean title-only edit is SILENT

  auto const task_res = dispatch(fx, {"task", "edit", "3"});
  CHECK(task_res.code == 0);
  CHECK(task_res.err.empty());

  // WITNESS 1: two children ran, and each was handed the TEMP copy rather
  // than the workbench file.
  auto const recorded = read_file(witness);
  REQUIRE(!recorded.empty());
  std::size_t spawns = 0;
  for (auto const line : std::views::split(recorded, '\n')) {
    std::string_view const path{line.begin(), line.end()};
    if (path.empty()) {
      continue;
    }
    ++spawns;
    INFO("editor argv: " << path);
    CHECK(path.starts_with((fx.root / "tmp" / "planar-edit-").string()));
    CHECK(!path.contains("/wb/"));
  }
  CHECK(spawns == 2);

  // WITNESS 2: the sentinel reached BOTH rows.
  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from plans where id = 2") == "EDITED-BY-STUB-4c19");
  CHECK(scalar(conn, "select title from tasks where id = 3") == "EDITED-BY-STUB-4c19");
}

TEST_CASE("a non-zero $EDITOR exit aborts a plan or task edit without writing", "[cmd][drafting][edit][spawn][plan][task]") {
  // WITNESS 3 on these two families.
  auto fx = make_fixture("plantaskabort");
  seed_hierarchy(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^title: .*/title: SHOULD-NOT-LAND/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n"
                                        "exit 3\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const plan_res = dispatch(fx, {"plan", "edit", "2"});
  CHECK(plan_res.code == 0); // an aborted edit is not a FAILURE
  CHECK(plan_res.err == "aborted: editor exited with code 3\n");

  auto const task_res = dispatch(fx, {"task", "edit", "3"});
  CHECK(task_res.code == 0);
  CHECK(task_res.err == "aborted: editor exited with code 3\n");

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select title from plans where id = 2") == "Child");
  CHECK(scalar(conn, "select title from tasks where id = 3") == "T repo");
}

TEST_CASE("a task status edit runs NO transition guard, so todo jumps straight to done", "[cmd][drafting][edit][task]") {
  // `apply_mutations` guards `scenario` and nothing else, and this is what
  // that means for `task` in particular: the editor path will move a task
  // `todo -> done` without passing through `doing`, and back again — edges
  // `task done` and the status lifecycle in the CLI reference describe as
  // ordered. Run against the oracle in both directions rather than inferred
  // from the guard's shape, because "the guard names scenario" and "the
  // other five are therefore unguarded" is exactly the kind of inference
  // this milestone keeps finding to be half-true.
  //
  // Gate 1 still bites: the front-matter parser rejects a status outside
  // `task`'s own set before this flow ever sees it. Unguarded is not
  // unvalidated.
  auto fx = make_fixture("taskstatus");
  seed_hierarchy(fx);

  auto const set_status = [&](std::string_view to) {
    write_script(fx.root / "stub-editor", std::format("#!/bin/sh\n"
                                                      "sed -e 's/^status: .*/status: {}/' \"$1\" > \"$1.new\"\n"
                                                      "mv \"$1.new\" \"$1\"\n",
                                                      to));
    fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();
    return dispatch(fx, {"task", "edit", "3"});
  };

  auto conn = open_db(fx);
  REQUIRE(scalar(conn, "select status from tasks where id = 3") == "todo");

  auto const forward = set_status("done");
  CHECK(forward.code == 0);
  CHECK(forward.err.empty());
  CHECK(scalar(conn, "select status from tasks where id = 3") == "done");

  auto const backward = set_status("todo");
  CHECK(backward.code == 0);
  CHECK(scalar(conn, "select status from tasks where id = 3") == "todo");

  // GATE 1, which is NOT skipped. An unknown status never reaches the
  // update at all; the parser refuses it and the row is untouched.
  auto const bogus = set_status("nonsense");
  CHECK(bogus.code == 1);
  CHECK(bogus.err.contains("failed to parse editor output for task 3: InvalidFieldValue"));
  CHECK(scalar(conn, "select status from tasks where id = 3") == "todo");
}

TEST_CASE("a non-title, non-status front-matter edit warns and is dropped, for plan and task",
          "[cmd][drafting][edit][plan][task]") {
  // The `[M4 limitation: ...]` arm, on a field only these two families
  // carry in front matter. `priority` is rendered for `task` and for no
  // other kind, so this is the one place the warning can be reached through
  // a field that is not shared with the sixteen cases above.
  auto fx = make_fixture("plantaskm4");
  seed_hierarchy(fx);

  write_script(fx.root / "stub-editor", "#!/bin/sh\n"
                                        "sed -e 's/^priority: .*/priority: 7/' \"$1\" > \"$1.new\"\n"
                                        "mv \"$1.new\" \"$1\"\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"task", "edit", "3"});
  CHECK(res.code == 0);
  CHECK(res.err == "[M4 limitation: only title and status mutations are applied; other fields are ignored]\n"
                   "aborted: no changes made\n");

  auto conn = open_db(fx);
  CHECK(scalar(conn, "select priority from tasks where id = 3") == "100");
}
