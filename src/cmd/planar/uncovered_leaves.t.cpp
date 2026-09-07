// @file uncovered_leaves.t.cpp
// @brief Black-box coverage for the eight `planar` (verb, subcommand)
// leaves task 6546 found unexercised by any C++ test: `artifact edit`,
// `artifact review`, `decision diff`, `decision view`, `scenario diff`,
// `scenario review`, `scenario view`, `workbench sync` (plan 996, task
// 6546, decision 1035).
//
// ## Confirming the premise
//
// Grepping every `*.t.cpp` under `src/cmd/` for these eight exact argv
// tuples (`{"artifact", "edit"}`, `{"decision", "diff"}`, ...) turns up
// NOTHING before this file. Their only prior occurrence anywhere in the
// C++ test tree is as leaf NAMES inside the pinned `schema` catalog string
// literal at `parity.t.cpp:811` -- a JSON blob, never a dispatch. Every
// sibling leaf in the same shared `editflow` quartet (`question` all four,
// `plan`/`task` all four, `decision edit`/`review`, `scenario edit`,
// `artifact view`/`diff`) already has a dispatched case in
// `drafting_leaves.t.cpp`; these eight are the gap decision 1035 needs
// closed before ~584 redundant Zig integration blocks can be retired.
//
// ## Why `run_pinned`, not the in-process `dispatch()` fixture
//
// `drafting_leaves.t.cpp` and its siblings build a `context` in-process and
// call handlers directly -- fast, but it never exercises the real CLI11
// parse or a real spawned `$EDITOR`. This task asked specifically for the
// shared black-box harness (`../parity_harness.hpp`'s `run_pinned` /
// `make_arena`, already proven at 33 retired cases): a real compiled
// `planar` binary, a real subprocess editor, real files on a real
// filesystem. That is the shape decision 1035 is retiring Zig coverage
// into, so the replacement should already be shaped like the thing it
// replaces.
//
// ## Capture discipline
//
// Every pinned literal below was captured by RUNNING the built C++ binary
// against a pinned scratch arena identical to `run_pinned`'s (`PLANAR_DB`,
// `PLANAR_HOME`, `PLANAR_CONFIG_PATH`, `PLANAR_LOCAL_HOME`,
// `PLANAR_WORKBENCH_ROOT`, `HOME` all redirected) and piping the captured
// bytes through a small script that emits the exact C++ string-literal
// form -- never hand-transcribed. The previous cycle on this task lost
// eight separate cases to hand-copy transcription errors; this is the
// mitigation. The only substitutions applied to a captured literal are the
// same two `normalize()` performs on the live value before comparing:
// the arena's own random root (`<ARENA>`) and an ISO-8601-with-millis
// timestamp (`<TS>`), both of which are the only two things that
// legitimately cannot be pinned literally run to run (arena roots are
// `steady_clock`-keyed per `make_arena`; every write stamps the wall
// clock).
//
// ## The fixture
//
// `seed_and_push` reuses, verbatim, the ELEVEN-STEP seed sequence
// `parity.t.cpp`'s "a seeded workbench feature tree is pinned" test
// already ran against the oracle and pinned byte-for-byte (task 6542's
// header there records the oracle-parity confirmation). Reusing an
// already-verified argv sequence rather than inventing a new one means
// these eight cases inherit that confirmation rather than adding a new,
// unverified fixture shape.

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

import std;

#include "parity_harness.hpp"

namespace {

using ::planar::cmd::parity::arena;
using ::planar::cmd::parity::capture;
using ::planar::cmd::parity::make_arena;
using ::planar::cmd::parity::read_all;
using ::planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists,
/// shared with `parity.t.cpp` in the same test target).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Replace every occurrence of `root` in `text` with `<ARENA>`.
///
/// Same substitution `parity.t.cpp`'s "a seeded workbench feature tree is
/// pinned" case performs, for the same reason: `make_arena` keys the root
/// off `steady_clock`, so it can never be pinned literally.
/// @param text The captured bytes.
/// @param root The arena root to scrub.
/// @return The scrubbed text.
auto normalize_arena(std::string_view text, const std::filesystem::path& root) -> std::string {
  std::string const needle = root.string();
  std::string       out;
  std::string_view  rest = text;
  for (;;) {
    auto const at = rest.find(needle);
    if (at == std::string_view::npos) {
      out.append(rest);
      return out;
    }
    out.append(rest.substr(0, at));
    out.append("<ARENA>");
    rest.remove_prefix(at + needle.size());
  }
}

/// @brief Replace every ISO-8601-with-milliseconds timestamp in `text`
/// with `<TS>`.
///
/// Every entity this quartet touches stamps `created_at`/`updated_at` at
/// wall-clock resolution, so a canonical-render diff or a `show` line that
/// carries one cannot be pinned literally without this.
/// @param text The captured bytes.
/// @return The scrubbed text.
auto normalize_stamps(std::string_view text) -> std::string {
  static std::regex const stamp{R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)"};
  return std::regex_replace(std::string(text), stamp, "<TS>");
}

/// @brief Apply both normalizations, in the order that matters least
/// because neither substitution can appear inside the other's match.
/// @param text The captured bytes.
/// @param root The arena root to scrub.
/// @return The scrubbed text.
auto normalize(std::string_view text, const std::filesystem::path& root) -> std::string {
  return normalize_stamps(normalize_arena(text, root));
}

/// @brief Write an executable shell script.
/// @param path Where to write it.
/// @param body The script body, `#!` line included.
auto write_script(const std::filesystem::path& path, std::string_view body) -> void {
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << body;
  }
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
  REQUIRE(!ec);
}

/// @brief Seed one arena with the exact eleven-step feature tree
/// `parity.t.cpp`'s "a seeded workbench feature tree is pinned" test
/// builds and already pinned against the oracle, then push it. See this
/// file's header for why reusing that sequence matters.
/// @param work The arena's cpp-side scratch root.
auto seed_and_push(const std::filesystem::path& work) -> void {
  auto const seed = std::to_array<std::vector<std::string>>({
      {"init", "--name", "demo", "--slug", "demo"},
      {"assoc", "create", "project:demo", "--kind", "project"},
      {"assoc", "add", "project:demo", (work / "proj").string()},
      {"plan", "create", "Demo Feature", "--slug", "demo-feature", "--summary", "A demo."},
      {"task", "add", "First Task", "--plan", "1", "--body", "Task body here.", "--editor=false"},
      {"task", "add", "Second Task", "--plan", "1", "--editor=false"},
      {"artifact", "add", "Tech Spec: Auth", "--kind", "tech_spec", "--plan", "1", "--body", "Spec body.", "--editor=false"},
      {"decision", "add", "Use SQLite", "--plan", "1", "--body", "We use SQLite.", "--rationale", "Simple."},
      {"question", "add", "Which format?", "--plan", "1"},
      {"scenario", "add", "Round trip", "--plan", "1"},
      {"plan", "create", "Child Milestone", "--slug", "child-ms", "--parent", "1"},
  });
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("seed{}", i);
    auto const ran = run_pinned(cpp_bin(), seed[i], work, tag);
    INFO("seed step: " << tag << " -> " << ran.err);
    REQUIRE(ran.code == 0);
  }
  REQUIRE(run_pinned(cpp_bin(), std::vector<std::string>{"workbench", "push", "1"}, work, "seedpush").code == 0);
}

/// @brief RAII: set an environment variable for the scope, restoring
/// whatever was there before (absent -> absent) on destruction.
///
/// `run_pinned` execs through `env VAR=... ...`, which sets only the
/// listed vars and INHERITS the rest of the calling process's environment
/// (`parity.t.cpp`'s `PLANAR_PARITY_STRICT` cases and `init.t.cpp`'s `PATH`
/// case already rely on exactly this). Setting `PLANAR_EDITOR` here before
/// a `run_pinned` call is therefore how a scripted, non-interactive editor
/// reaches the spawned child.
struct scoped_env {
  std::string        name;    ///< The variable name.
  std::optional<std::string> prior; ///< Its prior value, if it had one.

  explicit scoped_env(std::string var_name, std::string_view value) : name(std::move(var_name)) {
    if (char const* existing = std::getenv(name.c_str()); existing != nullptr) {
      prior = existing;
    }
    REQUIRE(::setenv(name.c_str(), std::string(value).c_str(), 1) == 0);
  }
  scoped_env(const scoped_env&)            = delete;
  scoped_env& operator=(const scoped_env&) = delete;
  ~scoped_env() {
    if (prior) {
      static_cast<void>(::setenv(name.c_str(), prior->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(name.c_str()));
    }
  }
};

} // namespace

// ===========================================================================
// artifact edit
// ===========================================================================

TEST_CASE("task 6546: artifact edit rewrites the title through a scripted $EDITOR and aborts cleanly otherwise",
          "[cmd][uncovered][artifact][edit]") {
  auto const space = make_arena("uncov_aedit");
  seed_and_push(space.cpp_root);

  auto const stub = space.cpp_root / "rewrite-title.sh";
  write_script(stub, "#!/bin/sh\n"
                      "sed -e 's/^title: .*/title: New Artifact Title/' \"$1\" > \"$1.new\"\n"
                      "mv \"$1.new\" \"$1\"\n"
                      "exit 0\n");

  {
    scoped_env const editor("PLANAR_EDITOR", stub.string());
    auto const edited = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "edit", "1"}, space.cpp_root, "aedit");
    CHECK(edited.code == 0);
    CHECK(edited.out.empty());
    CHECK(edited.err.empty()); // a clean title-only edit is SILENT
  }

  // Post-state read back via `show`, not exit code alone -- a no-op edit is
  // still exit 0.
  auto const shown = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "show", "1"}, space.cpp_root, "aeditshow");
  CHECK(shown.code == 0);
  CHECK(normalize_stamps(shown.out) == "id:          1\n"
                                       "title:       New Artifact Title\n"
                                       "kind:        tech_spec\n"
                                       "status:      draft\n"
                                       "scope:       association:1\n"
                                       "body:        Spec body.\n"
                                       "created:     <TS>\n"
                                       "updated:     <TS>\n");

  // A non-zero editor exit aborts WITHOUT writing -- exit 0, not a failure.
  auto const fail = space.cpp_root / "fail-editor.sh";
  write_script(fail, "#!/bin/sh\n"
                      "sed -e 's/^title: .*/title: SHOULD-NOT-LAND/' \"$1\" > \"$1.new\"\n"
                      "mv \"$1.new\" \"$1\"\n"
                      "exit 1\n");
  {
    scoped_env const editor("PLANAR_EDITOR", fail.string());
    auto const aborted = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "edit", "1"}, space.cpp_root, "aabort");
    CHECK(aborted.code == 0);
    CHECK(aborted.out.empty());
    CHECK(aborted.err == "aborted: editor exited with code 1\n");
  }
  auto const shown_after_abort =
      run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "show", "1"}, space.cpp_root, "aabortshow");
  CHECK(normalize_stamps(shown_after_abort.out).starts_with("id:          1\ntitle:       New Artifact Title\n"));

  // Refusal: a non-integer id, at exit 2, before any editor spawns.
  auto const bad_id = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "edit", "abc"}, space.cpp_root, "abadid");
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: artifact id must be an integer, got 'abc'\n");

  // Refusal: an artifact with no plan link reports NoPlanLink, unmapped,
  // exactly as `view` does (see `editflow.cppm`'s header).
  auto const no_link = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "edit", "999"}, space.cpp_root, "anolink");
  CHECK(no_link.code == 1);
  CHECK(no_link.err == "error: cannot resolve anchor plan for artifact 999: NoPlanLink\n"
                       "error: NoPlanLink\n");

  // Refusal: an unresolvable editor is a spawn failure, not an abort.
  {
    scoped_env const editor("PLANAR_EDITOR", "planar-no-such-editor-exists");
    auto const missing = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "edit", "1"}, space.cpp_root, "amissing");
    CHECK(missing.code == 1);
    CHECK(missing.out.empty());
    CHECK(missing.err == "error: EditorFailed\n");
  }
}

// ===========================================================================
// artifact review
// ===========================================================================

TEST_CASE("task 6546: artifact review reports the diff-shaped preview, the JSON envelope for both verdicts, and "
          "refuses",
          "[cmd][uncovered][artifact][review]") {
  auto const space = make_arena("uncov_areview");
  seed_and_push(space.cpp_root);

  // Clean: `review` with no verdict is `diff` in text form, and equal
  // content is silent.
  auto const clean = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "review", "1"}, space.cpp_root, "arclean");
  CHECK(clean.code == 0);
  CHECK(clean.out.empty());
  CHECK(clean.err.empty());

  auto const clean_json =
      run_pinned(cpp_bin(), std::array<std::string, 4>{"artifact", "review", "1", "--json"}, space.cpp_root, "arcleanjson");
  CHECK(clean_json.code == 0);
  CHECK(normalize_arena(clean_json.out, space.cpp_root) ==
        "{\"entity\":\"artifact\",\"id\":1,\"anchor_plan_id\":1,\"workbench_path\":\"<ARENA>/workbench/project_demo/"
        "p1-demo-feature/1-tech-spec-auth.md\",\"verdict\":null,\"has_changes\":false,\"persisted\":false,"
        "\"persistence\":\"none\"}\n");

  // `--approve`: text form names the verdict and the (always-false)
  // persistence, per the shared review body's contract -- there is no
  // per-entity review table.
  auto const approve =
      run_pinned(cpp_bin(), std::array<std::string, 4>{"artifact", "review", "1", "--approve"}, space.cpp_root, "arapprove");
  CHECK(approve.code == 0);
  CHECK(normalize_arena(approve.out, space.cpp_root) ==
        "artifact 1 review: approve (persisted: no; reason: no per-entity review table)\n"
        "workbench: <ARENA>/workbench/project_demo/p1-demo-feature/1-tech-spec-auth.md\n"
        "anchor_plan: 1\n"
        "changes_pending: no\n");

  // `--request-changes --json`.
  auto const request_changes = run_pinned(
      cpp_bin(), std::array<std::string, 5>{"artifact", "review", "1", "--request-changes", "--json"}, space.cpp_root, "arreqj");
  CHECK(request_changes.code == 0);
  CHECK(normalize_arena(request_changes.out, space.cpp_root) ==
        "{\"entity\":\"artifact\",\"id\":1,\"anchor_plan_id\":1,\"workbench_path\":\"<ARENA>/workbench/project_demo/"
        "p1-demo-feature/1-tech-spec-auth.md\",\"verdict\":\"request-changes\",\"has_changes\":false,\"persisted\":"
        "false,\"persistence\":\"none\"}\n");

  // Refusal: both verdicts together, at exit 2, BEFORE the database opens.
  auto const mutex = run_pinned(
      cpp_bin(), std::array<std::string, 5>{"artifact", "review", "1", "--approve", "--request-changes"}, space.cpp_root, "armutex");
  CHECK(mutex.code == 2);
  CHECK(mutex.out.empty());
  CHECK(mutex.err == "error: --approve and --request-changes are mutually exclusive\n");

  // Refusal: an unlinked id.
  auto const no_link =
      run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "review", "999"}, space.cpp_root, "arnolink");
  CHECK(no_link.code == 1);
  CHECK(no_link.err == "error: artifact 999 is not linked to a plan; cannot resolve anchor plan\n");

  // Refusal: `review`, unlike `view`/`edit`, is STRICT -- 0 is rejected.
  auto const bad_id = run_pinned(cpp_bin(), std::array<std::string, 3>{"artifact", "review", "0"}, space.cpp_root, "arbadid");
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: artifact id must be a positive integer, got 0\n");
}

// ===========================================================================
// decision view
// ===========================================================================

TEST_CASE("task 6546: decision view writes the canonical file and refuses an unresolvable anchor",
          "[cmd][uncovered][decision][view]") {
  auto const space = make_arena("uncov_dview");
  seed_and_push(space.cpp_root);

  auto const viewed = run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "view", "1"}, space.cpp_root, "dview");
  CHECK(viewed.code == 0);
  CHECK(viewed.err.empty());
  // No wall-clock content in the THIN render -- pinned whole, no normalize
  // needed (see `editflow.cppm`'s header: `view`/`edit` use the thin
  // renderer, `diff`/`review` the canonical one that carries the stamps).
  CHECK(viewed.out == "---\n"
                      "entity_kind: decision\n"
                      "entity_id: 1\n"
                      "anchor_plan_id: 1\n"
                      "title: Use SQLite\n"
                      "status: proposed\n"
                      "---\n"
                      "\n"
                      "# Decision 1: Use SQLite\n"
                      "\n"
                      "**Status:** proposed\n"
                      "\n"
                      "## Body\n"
                      "\n"
                      "We use SQLite.\n"
                      "\n"
                      "## Rationale\n"
                      "\n"
                      "Simple.\n");

  // The rendered file itself is the real product (view writes it as a side
  // effect, then spawns a pager over it) -- assert the bytes ON DISK too,
  // not only what reached this process's captured stdout.
  auto const rendered_path =
      space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature" / "decisions" / "1-use-sqlite.md";
  CHECK(read_all(rendered_path) == viewed.out);

  // Refusal: `view` is LENIENT about the id shape (0 parses), but an
  // unlinked/nonexistent decision still reports NoPlanLink, unmapped.
  auto const not_found = run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "view", "999"}, space.cpp_root, "dviewnf");
  CHECK(not_found.code == 1);
  CHECK(not_found.err == "error: cannot resolve anchor plan for decision 999: NoPlanLink\n"
                        "error: NoPlanLink\n");
}

// ===========================================================================
// decision diff
// ===========================================================================

TEST_CASE("task 6546: decision diff is silent when clean, pins the timestamp-only hunk when dirty, and refuses",
          "[cmd][uncovered][decision][diff]") {
  auto const space = make_arena("uncov_ddiff");
  seed_and_push(space.cpp_root);

  // Right after `push`, the canonical file already matches the database --
  // `diff` on equal content writes nothing and exits 0, mirroring `diff -u`.
  auto const clean = run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "diff", "1"}, space.cpp_root, "ddclean");
  CHECK(clean.code == 0);
  CHECK(clean.out.empty());
  CHECK(clean.err.empty());

  // `view` overwrites the file with the THIN render, which drops the two
  // `**Created:**`/`**Updated:**` lines the canonical render carries --
  // `diff` then reports exactly that two-line deletion.
  REQUIRE(run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "view", "1"}, space.cpp_root, "ddview").code == 0);
  auto const dirty = run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "diff", "1"}, space.cpp_root, "ddirty");
  CHECK(dirty.code == 0);
  CHECK(normalize(dirty.out, space.cpp_root) == "--- db:decision:1\n"
                                                "+++ fs:<ARENA>/workbench/project_demo/p1-demo-feature/decisions/"
                                                "1-use-sqlite.md\n"
                                                "@@ -8,9 +8,7 @@\n"
                                                " \n"
                                                " # Decision 1: Use SQLite\n"
                                                " \n"
                                                "-**Status:** proposed  \n"
                                                "-**Created:** <TS>  \n"
                                                "-**Updated:** <TS>\n"
                                                "+**Status:** proposed\n"
                                                " \n"
                                                " ## Body\n"
                                                " \n");
  CHECK(dirty.err.empty());

  // Refusal: `diff` is STRICT -- 0 is rejected at exit 2, unlike `view`.
  auto const bad_id = run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "diff", "0"}, space.cpp_root, "ddbadid");
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: decision id must be a positive integer, got 0\n");

  // Refusal: an unlinked id -- `diff`'s prose names the anchor failure
  // directly, unlike `view`'s bare `NoPlanLink` tag.
  auto const no_link = run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "diff", "999"}, space.cpp_root, "ddnolink");
  CHECK(no_link.code == 1);
  CHECK(no_link.err == "error: decision 999 is not linked to a plan; cannot resolve anchor plan\n");
}

// ===========================================================================
// scenario view
// ===========================================================================

TEST_CASE("task 6546: scenario view writes the canonical file and refuses an unresolvable anchor",
          "[cmd][uncovered][scenario][view]") {
  auto const space = make_arena("uncov_sview");
  seed_and_push(space.cpp_root);

  auto const viewed = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "view", "1"}, space.cpp_root, "sview");
  CHECK(viewed.code == 0);
  CHECK(viewed.err.empty());
  CHECK(viewed.out == "---\n"
                      "entity_kind: scenario\n"
                      "entity_id: 1\n"
                      "anchor_plan_id: 1\n"
                      "title: Round trip\n"
                      "status: draft\n"
                      "---\n"
                      "\n"
                      "# Scenario 1: Round trip\n"
                      "\n"
                      "**Status:** draft\n");

  auto const rendered_path =
      space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature" / "scenarios" / "1-round-trip.md";
  CHECK(read_all(rendered_path) == viewed.out);

  // Refusal: `scenario view 999` -- lenient id, unlinked entity, bare tag.
  auto const not_found = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "view", "999"}, space.cpp_root, "sviewnf");
  CHECK(not_found.code == 1);
  CHECK(not_found.err == "error: cannot resolve anchor plan for scenario 999: NoPlanLink\n"
                        "error: NoPlanLink\n");

  // Refusal: a non-integer id, at exit 2.
  auto const bad_id = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "view", "abc"}, space.cpp_root, "sviewbadid");
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: scenario id must be an integer, got 'abc'\n");
}

// ===========================================================================
// scenario diff
// ===========================================================================

TEST_CASE("task 6546: scenario diff is silent when clean and pins the timestamp-only hunk when dirty",
          "[cmd][uncovered][scenario][diff]") {
  auto const space = make_arena("uncov_sdiff");
  seed_and_push(space.cpp_root);

  auto const clean = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "diff", "1"}, space.cpp_root, "sdclean");
  CHECK(clean.code == 0);
  CHECK(clean.out.empty());
  CHECK(clean.err.empty());

  REQUIRE(run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "view", "1"}, space.cpp_root, "sdview").code == 0);
  auto const dirty = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "diff", "1"}, space.cpp_root, "sdirty");
  CHECK(dirty.code == 0);
  CHECK(normalize(dirty.out, space.cpp_root) == "--- db:scenario:1\n"
                                                "+++ fs:<ARENA>/workbench/project_demo/p1-demo-feature/scenarios/"
                                                "1-round-trip.md\n"
                                                "@@ -8,7 +8,5 @@\n"
                                                " \n"
                                                " # Scenario 1: Round trip\n"
                                                " \n"
                                                "-**Status:** draft  \n"
                                                "-**Created:** <TS>  \n"
                                                "-**Updated:** <TS>\n"
                                                "+**Status:** draft\n"
                                                " \n");
  CHECK(dirty.err.empty());

  // Refusal: strict-id and unlinked-id, same shapes as `decision diff`.
  auto const bad_id = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "diff", "0"}, space.cpp_root, "sdbadid");
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: scenario id must be a positive integer, got 0\n");

  auto const no_link = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "diff", "999"}, space.cpp_root, "sdnolink");
  CHECK(no_link.code == 1);
  CHECK(no_link.err == "error: scenario 999 is not linked to a plan; cannot resolve anchor plan\n");
}

// ===========================================================================
// scenario review
// ===========================================================================

TEST_CASE("task 6546: scenario review reports the diff, the JSON envelope for both verdicts, and refuses",
          "[cmd][uncovered][scenario][review]") {
  auto const space = make_arena("uncov_sreview");
  seed_and_push(space.cpp_root);

  // Clean, `--json`: `has_changes: false`, `verdict: null`.
  auto const clean_json =
      run_pinned(cpp_bin(), std::array<std::string, 4>{"scenario", "review", "1", "--json"}, space.cpp_root, "srcleanjson");
  CHECK(clean_json.code == 0);
  CHECK(normalize_arena(clean_json.out, space.cpp_root) ==
        "{\"entity\":\"scenario\",\"id\":1,\"anchor_plan_id\":1,\"workbench_path\":\"<ARENA>/workbench/project_demo/"
        "p1-demo-feature/scenarios/1-round-trip.md\",\"verdict\":null,\"has_changes\":false,\"persisted\":false,"
        "\"persistence\":\"none\"}\n");

  // `view` dirties the file the same way it does for `decision`, so
  // `--approve --json` after it reports `has_changes: true`.
  REQUIRE(run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "view", "1"}, space.cpp_root, "srview").code == 0);
  auto const approve_json = run_pinned(
      cpp_bin(), std::array<std::string, 5>{"scenario", "review", "1", "--approve", "--json"}, space.cpp_root, "srapprovejson");
  CHECK(approve_json.code == 0);
  CHECK(normalize_arena(approve_json.out, space.cpp_root) ==
        "{\"entity\":\"scenario\",\"id\":1,\"anchor_plan_id\":1,\"workbench_path\":\"<ARENA>/workbench/project_demo/"
        "p1-demo-feature/scenarios/1-round-trip.md\",\"verdict\":\"approve\",\"has_changes\":true,\"persisted\":false,"
        "\"persistence\":\"none\"}\n");

  // Refusal: both verdicts together.
  auto const mutex = run_pinned(
      cpp_bin(), std::array<std::string, 5>{"scenario", "review", "1", "--approve", "--request-changes"}, space.cpp_root, "srmutex");
  CHECK(mutex.code == 2);
  CHECK(mutex.err == "error: --approve and --request-changes are mutually exclusive\n");

  // Refusal: strict-id and unlinked-id.
  auto const bad_id = run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "review", "0"}, space.cpp_root, "srbadid");
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: scenario id must be a positive integer, got 0\n");

  auto const no_link =
      run_pinned(cpp_bin(), std::array<std::string, 3>{"scenario", "review", "999"}, space.cpp_root, "srnolink");
  CHECK(no_link.code == 1);
  CHECK(no_link.err == "error: scenario 999 is not linked to a plan; cannot resolve anchor plan\n");
}

// ===========================================================================
// workbench sync
// ===========================================================================

TEST_CASE("task 6546: workbench sync applies both directions in one pass, and reports not-found, invalid-plan and "
          "conflict refusals",
          "[cmd][uncovered][workbench][sync]") {
  auto const space = make_arena("uncov_wbsync");
  seed_and_push(space.cpp_root);

  // Diverge BOTH ways before the one `sync` call: a front-matter STATUS
  // edit on the filesystem (task 2, todo -> doing -- `status:` round-trips
  // for `task`, unlike every other kind; see `sync.cpp`'s `pull_to_db`),
  // and a status-only mutation in the database (`decision accept`,
  // proposed -> accepted) that never touched the pushed file.
  auto const task_file = space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature" / "tasks" / "cross" / "2-second-task.md";
  {
    auto const before = read_all(task_file);
    REQUIRE_FALSE(before.empty());
    std::string after = before;
    auto const  pos   = after.find("status: todo");
    REQUIRE(pos != std::string::npos);
    after.replace(pos, std::string_view{"status: todo"}.size(), "status: doing");
    std::ofstream out(task_file, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << after;
  }
  REQUIRE(run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "accept", "1"}, space.cpp_root, "wbaccept").code == 0);

  auto const synced =
      run_pinned(cpp_bin(), std::array<std::string, 4>{"workbench", "sync", "1", "--verbose"}, space.cpp_root, "wbsync");
  CHECK(synced.code == 0);
  CHECK(synced.err.empty());
  CHECK(synced.out == "workbench sync: plan 1 (demo-feature)\n"
                      "  applied DB->FS: project_demo/p1-demo-feature/decisions/1-use-sqlite.md\n"
                      "  applied FS->DB: project_demo/p1-demo-feature/tasks/cross/2-second-task.md\n");

  // Post-state, BOTH directions, read back rather than inferred from the
  // summary line: the FS status change landed in the DB, and the DB status
  // change landed in the file.
  auto const task_shown = run_pinned(cpp_bin(), std::array<std::string, 3>{"task", "show", "2"}, space.cpp_root, "wbtaskshow");
  CHECK(normalize_stamps(task_shown.out) == "id:          2\n"
                                            "title:       Second Task\n"
                                            "status:      doing\n"
                                            "priority:    100\n"
                                            "scope:       association:1\n"
                                            "plan:        1\n"
                                            "body:        \n"
                                            "created:     <TS>\n"
                                            "updated:     <TS>\n");

  auto const decision_file = space.cpp_root / "workbench" / "project_demo" / "p1-demo-feature" / "decisions" / "1-use-sqlite.md";
  CHECK(normalize_stamps(read_all(decision_file)) == "---\n"
                                                     "entity_kind: decision\n"
                                                     "entity_id: 1\n"
                                                     "anchor_plan_id: 1\n"
                                                     "title: Use SQLite\n"
                                                     "status: accepted\n"
                                                     "---\n"
                                                     "\n"
                                                     "# Decision 1: Use SQLite\n"
                                                     "\n"
                                                     "**Status:** accepted  \n"
                                                     "**Created:** <TS>  \n"
                                                     "**Updated:** <TS>\n"
                                                     "\n"
                                                     "## Body\n"
                                                     "\n"
                                                     "We use SQLite.\n"
                                                     "\n"
                                                     "## Rationale\n"
                                                     "\n"
                                                     "Simple.\n");

  // A second sync over the now-clean tree is a true no-op: 0 applied, 0
  // pending, 0 conflicts -- proving the first sync's writes actually
  // reconciled both sides rather than merely reporting that they would.
  auto const settled = run_pinned(cpp_bin(), std::array<std::string, 4>{"workbench", "sync", "1", "--json"}, space.cpp_root, "wbsettled");
  CHECK(settled.code == 0);
  CHECK(settled.out == "{\"applied\":0,\"pending\":0,\"conflicts\":0,\"malformed\":0,\"malformed_files\":[],"
                       "\"filtered\":0,\"pre_existing_terminal\":0,\"cleaned\":0,\"filter_mode\":\"failures\","
                       "\"entries\":[{\"class\":\"no_op\",\"file_path\":\"project_demo/p1-demo-feature/README.md\","
                       "\"entity_kind\":\"plan\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":"
                       "\"no_op\",\"file_path\":\"project_demo/p1-demo-feature/1-tech-spec-auth.md\",\"entity_kind\":"
                       "\"artifact\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
                       "\"file_path\":\"project_demo/p1-demo-feature/decisions/1-use-sqlite.md\",\"entity_kind\":"
                       "\"decision\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
                       "\"file_path\":\"project_demo/p1-demo-feature/questions/1-which-format.md\",\"entity_kind\":"
                       "\"question\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
                       "\"file_path\":\"project_demo/p1-demo-feature/scenarios/1-round-trip.md\",\"entity_kind\":"
                       "\"scenario\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
                       "\"file_path\":\"project_demo/p1-demo-feature/tasks/cross/1-first-task.md\",\"entity_kind\":"
                       "\"task\",\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
                       "\"file_path\":\"project_demo/p1-demo-feature/tasks/cross/2-second-task.md\",\"entity_kind\":"
                       "\"task\",\"entity_id\":2,\"conflict_id\":0,\"parse_error\":\"\"},{\"class\":\"no_op\","
                       "\"file_path\":\"project_demo/p1-demo-feature/plans/child-ms.md\",\"entity_kind\":\"plan\","
                       "\"entity_id\":2,\"conflict_id\":0,\"parse_error\":\"\"}]}\n");

  // Refusal: `sync` shares `run_sync_verb` with `push`/`pull`, so it shares
  // their plan-resolution refusals byte-for-byte -- not found, invalid
  // shape, and a non-anchor (child) plan.
  auto const not_found = run_pinned(cpp_bin(), std::array<std::string, 3>{"workbench", "sync", "999"}, space.cpp_root, "wbsyncnf");
  CHECK(not_found.code == 1);
  CHECK(not_found.err == "error: plan not found: 999\n");

  auto const invalid_shape = run_pinned(cpp_bin(), std::array<std::string, 3>{"workbench", "sync", "0"}, space.cpp_root, "wbsync0");
  CHECK(invalid_shape.code == 2);
  CHECK(invalid_shape.err == "error: invalid plan '0'\n");

  auto const child_plan = run_pinned(cpp_bin(), std::array<std::string, 3>{"workbench", "sync", "2"}, space.cpp_root, "wbsyncchild");
  CHECK(child_plan.code == 1);
  CHECK(child_plan.err == "error: plan not found: 2\n");

  // A genuine CONFLICT: the SAME decision entity edited differently on
  // both sides between syncs. `sync` reports it and exits 3, and does NOT
  // resolve it unilaterally -- the operator must `workbench resolve`.
  {
    auto const before = read_all(decision_file);
    std::string after = before;
    auto const  pos   = after.find("We use SQLite.");
    REQUIRE(pos != std::string::npos);
    after.replace(pos, std::string_view{"We use SQLite."}.size(), "We use SQLite for real.");
    std::ofstream out(decision_file, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << after;
  }
  REQUIRE(run_pinned(cpp_bin(), std::array<std::string, 3>{"decision", "withdraw", "1"}, space.cpp_root, "wbwithdraw").code == 0);

  auto const conflict =
      run_pinned(cpp_bin(), std::array<std::string, 4>{"workbench", "sync", "1", "--verbose"}, space.cpp_root, "wbconflict");
  CHECK(conflict.code == 3);
  CHECK(conflict.out == "workbench sync: plan 1 (demo-feature)\n"
                        "  CONFLICT [1]: project_demo/p1-demo-feature/decisions/1-use-sqlite.md (decision 1)\n"
                        "  1 conflict(s) - run 'workbench resolve <event-id> --prefer fs|db'\n");
  CHECK(conflict.err == "error: 1 conflict(s) require 'workbench resolve <event-id> --prefer fs|db'\n");
}
