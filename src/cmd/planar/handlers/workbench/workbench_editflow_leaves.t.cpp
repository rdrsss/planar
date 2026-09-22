// @file workbench_editflow_leaves.t.cpp
// @brief Leaf-level tests for `workbench edit` and `workbench
// extract-questions` (plan 996, task 6302).
//
// Same shape as `drafting_leaves.t.cpp`: dispatch the real tree and table
// against a scratch root, then assert the operator-visible output AND the
// durable state.
//
// ## THE $EDITOR PATTERN IS REUSED, WITH ONE DELIBERATE DIFFERENCE
//
// `drafting_leaves.t.cpp` established the stub-script mechanism — a shell
// script in the fixture root, reached through `PLANAR_EDITOR` in the
// fixture's environment map, resolved via `context::env()` rather than
// `std::getenv` (`src/cmd/planar/CMakeLists.txt` records `process_env()` as
// the only permitted `getenv` caller in this target). That carries over
// unchanged and is not redesigned here.
//
// WHAT DOES NOT CARRY OVER IS THE ARGV WITNESS'S EXPECTATION. The drafting
// quartet hands the editor a TEMP file and writes back, so its witness
// asserts the recorded path is under `$TMPDIR` and is NOT the workbench
// file. `workbench edit` is the opposite verb: it hands the editor the
// FEATURE DIRECTORY itself and reconciles afterwards with `pull`. Oracle-
// probed — the stub's `$1` is
// `<root>/project_demo/p1-demo-feature`. Asserting a temp path here would
// have been a confident, wrong assertion carried over from a sibling, which
// is the failure mode this milestone keeps hitting.
//
// THREE WITNESSES, each red on its own if the spawn stops:
//
//   1. THE ARGV WITNESS. The stub appends `$1`; the recorded path must be
//      the feature directory, and that directory must exist.
//   2. THE PROVENANCE WITNESS, the load-bearing one. The stub rewrites a
//      line of a rendered artifact's BODY to a sentinel appearing NOWHERE
//      in `src/`. The sentinel can only reach the `artifacts` row by way of
//      a spawned process editing a file the following `pull` read back. A
//      port that skipped the spawn would report a clean pull and leave the
//      body alone.
//
//      IT IS THE BODY, NOT THE TITLE, and that distinction is load-bearing
//      rather than arbitrary. `sync::pull_to_db` writes `body` (plus
//      `status`, for `task` and `plan` only) and writes `title` for NO
//      kind. The first draft of that case rewrote `title:`, carried over
//      from the drafting quartet where the editflow does write titles, and
//      failed with the row unchanged while `pull` reported `1 applied`. A
//      witness aimed at a field the verb never writes cannot distinguish a
//      live spawn from a dead one.
//   3. THE EXIT-CODE WITNESS. A stub that mutates the file and exits 7 must
//      abort at exit 1 with the oracle's message, and the row must be
//      UNCHANGED — the `pull` never runs.
//
// BREAK-PROBES, run via `scripts/break-probe.sh` rather than assumed. Each
// mutant built and was KILLED by the single named test; no survivors:
//
//   editor: `spawn_inherit` returns 0 without forking
//       -> "the workbench edit spawn is real, and the editor gets the
//           FEATURE DIRECTORY"                                     killed
//       -> "an editor that rewrites a spec's body lands that text
//           in the row"                                            killed
//   workbench edit: drop the `*status != 0` guard
//       -> "a non-zero editor exit aborts before the pull"         killed
//   extract-questions: report the malformed file instead of skipping
//       -> "extract-questions SWALLOWS a malformed spec"           killed
//   questions: stop skipping README.md
//       -> "extract-questions skips README.md and does not descend"  killed
//   dispatch: unwire `workbench extract-questions`
//       -> "every leaf is in exactly one of the two handler
//           populations"                                           killed
//
// ## THE FIXTURE FOR extract-questions IS BUILT BY A REAL PUSH
//
// This is the single most important thing in this file. `extract-questions`
// reads ONLY top-level `.md` files and skips `README.md`. A feature tree
// that is a README plus subdirectories therefore yields `[]` — on both
// binaries — and every comparison passes while nothing is tested. A
// hand-authored `.md` does not rescue it either: the file must satisfy
// `workbench::parse::parse`, and a malformed one is SWALLOWED (see below),
// so a fixture written by hand tends to produce the same empty result for a
// second, different reason.
//
// So the fixture here is produced by seeding a real artifact and running a
// real `workbench push`, and every case asserts the extraction is NON-EMPTY
// before comparing anything.
//
// ## THE MALFORMED-FILE SWALLOW IS REPRODUCED ON PURPOSE
//
// `parse(...)` failing means `continue`: the same file `workbench push`
// rejects as `MissingRequiredField` at exit 1 is skipped silently here, so
// a spec with broken front matter reports zero questions at exit 0. That is
// the oracle's behaviour, it is reproduced under D2, and it is pinned by a
// case below so a future change to it is a deliberate one. Filed as planar
// task 6304.
//
// ## Every expectation here came from RUNNING the oracle
//
// Captured against `zig/zig-out/bin/planar` in a pinned scratch arena, every
// invocation piped on BOTH streams with the exit code read outside the pipe
// (see `../parity_harness.hpp` for why an unpiped capture silently
// corrupts). The full transcript diffed byte-for-byte against this
// implementation before these cases were written.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

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
/// `PATH` is carried through from the real environment because the stub
/// editor is a real program that has to be found; it is the one variable
/// here that is not scratch, and it is read-only. Nothing reads the
/// operator's `~/.planar`.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_wbedit_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "wb", ec);

  auto const* path = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe)
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()},
                  {"PLANAR_WORKBENCH_ROOT", (root / "wb").string()},
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

/// @brief Read a whole file, or the empty string when absent.
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

/// @brief The artifact body carrying a bullet-shaped Open Questions section.
constexpr std::string_view k_body = R"(Intro line.

## Open Questions

- Should we cache the result? It would help a lot on repeated reads and we think it matters.
- What about eviction?

## Next Section

- not a question
)";

/// @brief Seed a project, an anchor plan and one artifact with questions.
///
/// Goes through the CLI rather than raw SQL so the fixture exercises the
/// same paths an operator would.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--name", "demo", "--slug", "demo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:demo", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:demo", (fx.root / "proj").string()}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Demo Feature", "--slug", "demo-feature", "--summary", "A demo.", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Bullet Spec", "--kind", "tech_spec", "--plan", "1", "--body", std::string{k_body},
                        "--editor=false", "--json"})
              .code == 0);
}

/// @brief The feature directory the seeded plan renders into.
auto feature_dir(const fixture& fx) -> std::filesystem::path {
  return fx.root / "wb" / "project_demo" / "p1-demo-feature";
}

/// @brief One scalar from a read-only query.
auto scalar(const fixture& fx, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  if (*stepped == planar::db::step_result::done) {
    return {};
  }
  return std::string{stmt->column_text(0)};
}

} // namespace

// ===========================================================================
// extract-questions
// ===========================================================================

TEST_CASE("extract-questions reports a pushed spec's questions, and the fixture is NOT empty",
          "[cmd][workbench][extract-questions]") {
  auto const fx = make_fixture("eqtext");
  seed(fx);
  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  // THE ANTI-VACUITY GUARD, asserted before the verb runs at all: the tree
  // must actually contain a top-level non-README spec. A feature tree that
  // is a README plus subdirectories makes this leaf return `[]` and every
  // assertion below pass against nothing.
  REQUIRE(std::filesystem::exists(feature_dir(fx) / "1-bullet-spec.md"));

  auto const res = dispatch(fx, {"workbench", "extract-questions", "1"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());
  // Oracle-captured verbatim. The elided body is 61 bytes, cut at 60.
  CHECK(res.out == "1-bullet-spec.md (artifact 1): 2 question(s)\n"
                   "  [line 12] Should we cache the result? — It would help a lot on repeated reads and we think it matter…\n"
                   "  [line 13] What about eviction?\n");
}

TEST_CASE("extract-questions --json emits the oracle's field order", "[cmd][workbench][extract-questions]") {
  auto const fx = make_fixture("eqjson");
  seed(fx);
  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  auto const res = dispatch(fx, {"workbench", "extract-questions", "1", "--json"});
  CHECK(res.code == 0);
  // Non-empty in the JSON dimension too: an empty `questions` array here
  // would be the vacuous shape wearing a different mask.
  REQUIRE(res.out.contains(R"("title":"Should we cache the result?")"));
  CHECK(
      res.out ==
      R"([{"artifact_id":1,"file":"1-bullet-spec.md","questions":[)"
      R"({"title":"Should we cache the result?","body":"It would help a lot on repeated reads and we think it matters.","source_line":12},)"
      R"({"title":"What about eviction?","body":"","source_line":13}]}])"
      "\n");
}

TEST_CASE("extract-questions skips README.md and does not descend into subdirectories", "[cmd][workbench][extract-questions]") {
  auto const fx = make_fixture("eqscope");
  seed(fx);
  // A question entity renders into `questions/`, giving the tree a real
  // subdirectory to ignore.
  REQUIRE(dispatch(fx, {"question", "add", "Which format?", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  // Both distractors exist, so the exclusions below are asserted against a
  // tree that actually contains what they exclude.
  REQUIRE(std::filesystem::exists(feature_dir(fx) / "README.md"));
  REQUIRE(std::filesystem::is_directory(feature_dir(fx) / "questions"));

  auto const res = dispatch(fx, {"workbench", "extract-questions", "1"});
  CHECK(res.code == 0);
  // Exactly one file line: the top-level spec. Neither README.md nor the
  // nested question file is reported.
  CHECK(res.out.starts_with("1-bullet-spec.md (artifact 1): 2 question(s)\n"));
  CHECK_FALSE(res.out.contains("README.md"));
  CHECK_FALSE(res.out.contains("which-format"));
}

TEST_CASE("extract-questions WARNS on a malformed spec that push would refuse, instead of swallowing it",
          "[cmd][workbench][extract-questions][6304]") {
  // Was: "extract-questions SWALLOWS a malformed spec ... push would
  // refuse", pinning the oracle's `parse(...) catch continue` under D2.
  // Decision 1116 (task 6304) fixed the C++ tree only: the same file
  // `push` rejects at exit 1 used to produce an indistinguishable `[]` at
  // exit 0 here. It still exits 0 — this is a read-only reporting leaf and
  // one bad file must not fail the whole command — but the file is now
  // named on stderr instead of disappearing.
  auto const fx = make_fixture("eqmalformed");
  seed(fx);
  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  // Break the front matter of the one spec the tree has.
  auto const spec = feature_dir(fx) / "1-bullet-spec.md";
  REQUIRE(std::filesystem::exists(spec));
  {
    std::ofstream file(spec, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << "---\nentity_kind: artifact\n---\n\n## Open Questions\n\n- Is this reported?\n";
  }

  // `pull` REFUSES it — the contrast that makes the next assertion mean
  // something. Without this half, "extract-questions reported nothing"
  // would be indistinguishable from "the file was fine and had no
  // questions".
  auto const pulled = dispatch(fx, {"workbench", "pull", "1"});
  CHECK(pulled.code == 1);

  // ...and extract-questions now NAMES the file on stderr rather than
  // reporting it as absent. The question content still does not appear in
  // the results: the CONTENT contract is unchanged, only its visibility.
  auto const res = dispatch(fx, {"workbench", "extract-questions", "1"});
  CHECK(res.code == 0);
  CHECK(res.out.empty());
  CHECK_FALSE(res.out.contains("Is this reported?"));
  CHECK(res.err.contains("1-bullet-spec.md"));
  CHECK(res.err.starts_with("warning: parsing"));
}

TEST_CASE("extract-questions on an unpushed plan hints instead of failing", "[cmd][workbench][extract-questions]") {
  auto const fx = make_fixture("eqnotree");
  seed(fx);
  REQUIRE(dispatch(fx, {"plan", "create", "Unpushed", "--slug", "unpushed", "--json"}).code == 0);

  auto const res = dispatch(fx, {"workbench", "extract-questions", "2"});
  CHECK(res.code == 0);
  CHECK(res.out == "workbench tree not found for plan 2; run 'workbench push 2' first\n");
}

TEST_CASE("extract-questions separates a missing plan from an invalid one", "[cmd][workbench][extract-questions]") {
  auto const fx = make_fixture("eqbadplan");
  seed(fx);

  auto const missing = dispatch(fx, {"workbench", "extract-questions", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: plan not found: 999\n");

  // `0` is INVALID, not missing — a different exit code, which is why both
  // arms are pinned rather than one standing for the pair.
  auto const invalid = dispatch(fx, {"workbench", "extract-questions", "0"});
  CHECK(invalid.code == 2);
  CHECK(invalid.err == "error: invalid plan '0'\n");
}

// ===========================================================================
// edit — and the $EDITOR spawn
// ===========================================================================

TEST_CASE("the workbench edit spawn is real, and the editor gets the FEATURE DIRECTORY", "[cmd][workbench][edit][spawn]") {
  // WITNESS 1. Note what is asserted: the feature directory, NOT a temp
  // path. See this file's header — the drafting quartet's witness asserts
  // the opposite, and carrying it over would have been wrong.
  auto fx = make_fixture("editargv");
  seed(fx);

  auto const witness = fx.root / "editor-witness";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\nprintf '%s\\n' \"$1\" >> {}\nexit 0\n", witness.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"workbench", "edit", "1"});
  CHECK(res.code == 0);
  CHECK(res.out == "workbench push: plan 1 (demo-feature) - 2 applied, 0 pending, 0 filtered (mode=failures), 0 conflict(s)\n"
                   "workbench pull: plan 1 (demo-feature) - 0 applied, 0 pending, 0 conflict(s)\n");

  auto const recorded = read_file(witness);
  REQUIRE(!recorded.empty()); // <-- the spawn happened at all.
  CHECK(recorded == feature_dir(fx).string() + "\n");
  // And it is a real directory the push had already created, not a path
  // that merely looks right.
  CHECK(std::filesystem::is_directory(feature_dir(fx)));
}

TEST_CASE("an editor that rewrites a spec's body lands that text in the row", "[cmd][workbench][edit][spawn]") {
  // WITNESS 2, the load-bearing one. The sentinel appears nowhere in
  // `src/`, so it can only reach the row through a spawned process writing
  // a file that the trailing `pull` read back.
  //
  // IT IS THE BODY, NOT THE TITLE, and that is not a stylistic choice.
  // `pull_to_db` writes `body` (and, for `task`/`plan` only, `status`); it
  // never writes `title` for ANY kind. The first draft of this case
  // rewrote `title:` — carried over from the drafting quartet, where the
  // editflow does write titles — and failed with the row still reading
  // "Bullet Spec" while `pull` cheerfully reported `1 applied`. A witness
  // aimed at a field the verb does not write is indistinguishable from a
  // dead spawn.
  auto fx = make_fixture("editprov");
  seed(fx);
  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  auto const spec = feature_dir(fx) / "1-bullet-spec.md";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\n"
                                                    "sed -i '' 's/^Intro line\\./XYZZY-EDIT-PROVENANCE/' {}\n"
                                                    "exit 0\n",
                                                    spec.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"workbench", "edit", "1"});
  CHECK(res.code == 0);
  // The pull applied exactly one file.
  CHECK(res.out.contains("workbench pull: plan 1 (demo-feature) - 1 applied"));
  // THE ROW, not just the exit code.
  auto const body = scalar(fx, "select body from artifacts where id = 1");
  CHECK(body.contains("XYZZY-EDIT-PROVENANCE"));
  CHECK_FALSE(body.contains("Intro line."));
}

TEST_CASE("a non-zero editor exit aborts before the pull, leaving the row alone", "[cmd][workbench][edit][spawn]") {
  // WITNESS 3. The stub mutates the file AND exits 7. The mutation must NOT
  // land, because the pull never runs.
  auto fx = make_fixture("editfail");
  seed(fx);
  REQUIRE(dispatch(fx, {"workbench", "push", "1"}).code == 0);

  // The mutation targets the BODY, for the reason the provenance case
  // above records: `pull` never writes `title`, so a title-based assertion
  // here would hold whether or not the pull ran and would witness nothing.
  auto const spec = feature_dir(fx) / "1-bullet-spec.md";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\n"
                                                    "sed -i '' 's/^Intro line\\./SHOULD-NOT-LAND/' {}\n"
                                                    "exit 7\n",
                                                    spec.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"workbench", "edit", "1"});
  CHECK(res.code == 1);
  CHECK(res.err == std::format("error: editor \"{}\" exited with status 7\n", (fx.root / "stub-editor").string()));
  // The push summary still printed; the pull one did not.
  CHECK(res.out.contains("workbench push: plan 1"));
  CHECK_FALSE(res.out.contains("workbench pull"));
  // THE STUB DID RUN — so this case cannot pass by the spawn never
  // happening, which is the way an exit-code assertion most easily goes
  // green for the wrong reason.
  CHECK(read_file(spec).contains("SHOULD-NOT-LAND"));
  // ...and the row is untouched, because the pull never ran.
  auto const body = scalar(fx, "select body from artifacts where id = 1");
  CHECK_FALSE(body.contains("SHOULD-NOT-LAND"));
  CHECK(body.contains("Intro line."));
}

TEST_CASE("an unresolvable editor is reported as FileNotFound, not as an exit status", "[cmd][workbench][edit][spawn]") {
  // The two failures a fallback chain must distinguish: "could not exec"
  // versus "ran and exited non-zero". They carry different prose.
  auto fx = make_fixture("editnoexe");
  seed(fx);
  fx.vars["PLANAR_EDITOR"] = (fx.root / "definitely-not-here").string();

  auto const res = dispatch(fx, {"workbench", "edit", "1"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: running editor failed: FileNotFound\n");
  CHECK(res.out.contains("workbench push: plan 1"));
}

TEST_CASE("workbench edit --editor overrides the environment", "[cmd][workbench][edit][spawn]") {
  auto fx = make_fixture("editflag");
  seed(fx);

  auto const witness = fx.root / "editor-witness";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\nprintf '%s\\n' \"$1\" >> {}\nexit 0\n", witness.string()));
  // PLANAR_EDITOR points at something unusable, so a port that ignored the
  // flag would fail rather than silently pass.
  fx.vars["PLANAR_EDITOR"] = (fx.root / "definitely-not-here").string();

  auto const res = dispatch(fx, {"workbench", "edit", "1", "--editor", (fx.root / "stub-editor").string()});
  CHECK(res.code == 0);
  CHECK(!read_file(witness).empty());
}

TEST_CASE("workbench edit --json emits two sync payloads, push then pull", "[cmd][workbench][edit]") {
  auto fx = make_fixture("editjson");
  seed(fx);
  write_script(fx.root / "stub-editor", "#!/bin/sh\nexit 0\n");
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const res = dispatch(fx, {"workbench", "edit", "1", "--json"});
  CHECK(res.code == 0);
  // TWO documents, one per line — the push's and the pull's. A port that
  // printed one would still be valid JSON and still exit 0.
  auto const lines = std::ranges::count(res.out, '\n');
  CHECK(lines == 2);
  CHECK(res.out.starts_with(R"({"applied":2,)"));
}

TEST_CASE("workbench edit separates a missing plan from an invalid one, before spawning anything", "[cmd][workbench][edit]") {
  auto fx = make_fixture("editbadplan");
  seed(fx);

  auto const witness = fx.root / "editor-witness";
  write_script(fx.root / "stub-editor", std::format("#!/bin/sh\nprintf '%s\\n' \"$1\" >> {}\nexit 0\n", witness.string()));
  fx.vars["PLANAR_EDITOR"] = (fx.root / "stub-editor").string();

  auto const missing = dispatch(fx, {"workbench", "edit", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: plan not found: 999\n");

  auto const invalid = dispatch(fx, {"workbench", "edit", "0"});
  CHECK(invalid.code == 2);
  CHECK(invalid.err == "error: invalid plan '0'\n");

  // Neither arm reached the editor. The plan is resolved first, so a bad
  // argument never opens a directory in `$EDITOR`.
  CHECK(read_file(witness).empty());
}
