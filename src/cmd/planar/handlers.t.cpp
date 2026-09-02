// @file handlers.t.cpp
// @brief In-process tests for the ported verb handlers (plan 996, tasks
// 6105 and 6106).
//
// These run the REAL handlers through the REAL dispatch, with no
// subprocess: that is what the explicit-`context` design buys, and it is
// why a case can assert something a shelled binary cannot expose at all —
// `context::db_opened()`, i.e. whether a verb touched SQLite.
//
// HOME / DB SAFETY. Every case builds its own environment map over a
// unique scratch root. `PLANAR_HOME` is the only variable the workflow
// leaves read, and it is always a scratch directory; the database path is
// passed to the context directly and is always under that same root.
// Nothing here calls std::getenv, so there is no inherited-environment path
// by which the operator's ~/.planar could be reached — the failure mode
// commit 3ec6c37 fixed one layer down.
//
// ORACLE PROVENANCE. Every expected string below is a verbatim
// transcription of bytes the Zig reference binary wrote under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/PLANAR_LOCAL_HOME, read back
// through `python3 -c "print(repr(open(f,'rb').read()))"`.
//
//   $Z workflow list                (no workflows/ directory at all)
//     exit 0, stdout b'no shipped + sandbox workflows found\n'
//   $Z workflow list --json         (same fixture)
//     exit 0, stdout b''            <-- ZERO BYTES, not '[]', not '\n'
//   $Z workflow show nope
//     exit 1, stdout b'', stderr b"error: workflow 'nope' not found\n"
//   $Z annotate list --json         (empty database)
//     exit 0, stdout b'[]\n'        <-- terminated, unlike workflow's
//   $Z annotate list                (empty database)
//     exit 0, stdout b'(no annotations)\n'
//   $Z annotate add --anchor-path src/foo.zig --line-start 3 --line-end 7 \
//                   --title T1 --body B1 --tags "a, b,a"
//     exit 0, stdout b'id:          1\ntitle:       T1\nstatus:      active\n
//                      scope:       global\nanchor path: src/foo.zig\n
//                      anchor line: 3-7\ntags:        a, b\nbody:        B1\n
//                      created:     <ts>\nupdated:     <ts>\n'
//   $Z annotate add --anchor-path src/bar.zig --line-start 1 --title T2 --json
//     exit 0, stdout b'{"id":2,"scope_kind":"global","scope_id":null,
//                      "anchor":{"path":"src/bar.zig","line_start":1,
//                      "line_end":null,"commit_sha":"","text_hash":"",
//                      "text":""},"title":"T2","slug":null,"body":"",
//                      "status":"active","vendor":"","plan_id":null,
//                      "task_id":null,"tags":[],"created_at":"<ts>",
//                      "updated_at":"<ts>"}\n'
//   $Z annotate add                 (no --anchor-path)
//     exit 2, stdout b'', stderr b'error: --anchor-path is required\n'
//
// Task 6106's leaves, captured the same way. `unlink`'s success cases were
// seeded by running the ORACLE's own `init` + `ext register github gh
// --project owner/repo` + `link plan:1 --to gh:42` first, because neither
// of those verbs is ported:
//
//   $Z unlink 999
//     exit 1, stdout b'', stderr b'error: link 999 not found\n'
//   $Z unlink abc
//     exit 2, stdout b'', stderr b"error: invalid link id 'abc'\n"
//     …AND $PLANAR_DB was created and migrated before the refusal.
//   $Z unlink 1
//     exit 0, stdout b'unlinked: external link 1 removed\n'
//   $Z unlink 2 --json
//     exit 0, stdout b'{"ok":true,"id":2}\n'
//   sqlite3 $PLANAR_DB 'select prefix, body from session_entries'
//     action|unlink: removed external link 1
//     action|unlink: removed external link 2   <- on a session `ensure_active`
//                                                created; there was none before
//   $Z unlink 1_0     -> exit 1, b'error: link 10 not found\n'  (SEPARATORS)
//   $Z unlink 007     -> exit 1, b'error: link 7 not found\n'
//   $Z unlink _10     -> exit 2, b"error: invalid link id '_10'\n"
//   $Z workspace doctor          (empty database) -> exit 0, stdout b''
//   $Z workspace doctor --json   (empty database) -> exit 0, b'{"orgs":[]}\n'
//   $Z workspace doctor          (one healthy org)
//     exit 0, stdout b'org:acme ok\n'
//   $Z workspace doctor --json   (same)
//     b'{"orgs":[{"slug":"acme","org_id":1,"issues_found":0,
//       "issues_repaired":[]}]}\n'
//   $Z skills   -> exit 0, its own help page on stdout (see the case below)
//
// Timestamps are the only fields not asserted literally — they are wall
// clock. Everything around them is.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.external;
import planar.engine.identity;
import planar.db.migrate;
import planar.engine.workbench.fsutil;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A fixture root: a scratch directory plus the environment and
/// database path every case in this file dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_h_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
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
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

/// @brief Write a shipped workflow file into the fixture's PLANAR_HOME.
/// @param fx The fixture.
/// @param filename The file's base name.
/// @param content The file's bytes.
auto write_shipped_workflow(const fixture& fx, std::string_view filename, std::string_view content) -> void {
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "home" / "workflows", ec);
  std::ofstream file(fx.root / "home" / "workflows" / std::string{filename}, std::ios::binary);
  file << content;
}

/// @brief A string appearing NOWHERE else in `src/`, so its presence in a
/// witness file proves the `workflow run` stub actually executed. A handler
/// that faked success cannot invent it. Deliberately not derived from any
/// constant under test.
constexpr std::string_view k_run_sentinel = "zqEXECUTE-PROV-4c81-plan996-6272-xqz";

/// @brief Write the `planar-execute` stub used by the `workflow run` cases.
///
/// Records the sentinel and then one line per received argument to
/// `witness`, then exits with `code`. Those are witnesses 1-3 from the
/// block comment above the `workflow run` cases.
/// @param path Where to write the script.
/// @param witness The file the stub records its argv into.
/// @param code The exit status the stub should report.
auto write_run_stub(const std::filesystem::path& path, const std::filesystem::path& witness, int code) -> void {
  {
    std::ofstream out(path, std::ios::binary);
    out << std::format("#!/bin/sh\n"
                       "printf '%s\\n' '{}' > '{}'\n"
                       "for a in \"$@\"; do printf '%s\\n' \"$a\" >> '{}'; done\n"
                       "exit {}\n",
                       k_run_sentinel, witness.string(), witness.string(), code);
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec);
}

/// @brief Read a witness file whole, or the empty string when absent.
/// @param path The witness path.
/// @return Its bytes.
auto slurp_witness(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("version writes the build line and exits 0 without opening the database", "[cmd][handlers]") {
  auto const fx  = make_fixture("version");
  auto const got = dispatch(fx, {"version"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);
  // The one deliberate divergence in the subset: `cxx <compiler>` where the
  // oracle emits `zig <zig-version>` (`planar dev dev zig 0.16.0`).
  CHECK(got.out.starts_with("planar dev dev cxx "));
  CHECK(got.out.ends_with("\n"));

  // Task 6117, CLOSED. This used to assert SIX tokens and carry a comment
  // recording that `planar.cliapp.version`'s header promised five while
  // `compiler_version_string()` returned `Clang 22.1.8` — a space — and so
  // handed a script reading field 5 the word `Clang` where the oracle hands
  // it `0.16.0`. The separator is now a hyphen, so the promise holds:
  // `planar dev dev cxx Clang-22.1.8` splits into five, exactly like the
  // oracle's `planar dev dev zig 0.16.0`.
  auto const tokens = std::ranges::count(got.out, ' ') + 1;
  CHECK(tokens == 5);
  // The guard that keeps it that way: whatever toolchain builds this, the
  // identifier it reports must not reintroduce whitespace.
  CHECK(got.out.find(' ', got.out.find("cxx ") + 4) == std::string::npos);
}

TEST_CASE("workflow list on an empty catalog names the sources it searched", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("wlempty");
  auto const got = dispatch(fx, {"workflow", "list"});
  CHECK(got.code == 0);
  CHECK(got.out == "no shipped + sandbox workflows found\n");
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);
}

TEST_CASE("workflow list --json on an empty catalog emits ZERO BYTES", "[cmd][handlers][parity][terminator]") {
  // The sharpest edge of the terminator contract. A handler that appended
  // a newline to renderer output — the obvious, tidy-looking thing — emits
  // one byte here and breaks parity in a way no renderer change can fix.
  // Contrast with `annotate list --json` below, which emits '[]\n'.
  auto const fx  = make_fixture("wljson");
  auto const got = dispatch(fx, {"workflow", "list", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out.empty());
  CHECK(got.out.size() == 0);
  CHECK(got.err.empty());
}

TEST_CASE("workflow list renders the discovered catalog", "[cmd][handlers][parity]") {
  auto const fx = make_fixture("wlfull");
  write_shipped_workflow(fx, "finalize_closeout.lua",
                         "--[[ @meta\n"
                         "name: finalize-closeout\n"
                         "description: Deterministic closeout gate.\n"
                         "phases: closeout\n"
                         "--]]\n");
  write_shipped_workflow(fx, "bare.lua", "-- no meta here\n");

  auto const got = dispatch(fx, {"workflow", "list"});
  CHECK(got.code == 0);
  CHECK(got.out == "name                      kind      phases                description\n"
                   "bare                      shipped                         \n"
                   "finalize-closeout         shipped   closeout              Deterministic closeout gate.\n");
  CHECK_FALSE(got.db_open);
}

TEST_CASE("workflow show renders a hit and exits 0", "[cmd][handlers][parity]") {
  auto const fx = make_fixture("wsok");
  write_shipped_workflow(fx, "finalize_closeout.lua",
                         "--[[ @meta\n"
                         "name: finalize-closeout\n"
                         "description: Deterministic closeout gate.\n"
                         "phases: closeout\n"
                         "--]]\n");
  auto const got = dispatch(fx, {"workflow", "show", "finalize-closeout"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == std::format("name:        finalize-closeout\n"
                               "kind:        shipped\n"
                               "path:        {}\n"
                               "meta:        present\n"
                               "description: Deterministic closeout gate.\n"
                               "phases:      closeout\n",
                               (fx.root / "home" / "workflows" / "finalize_closeout.lua").string()));
}

TEST_CASE("workflow show on a miss exits 1 with the complete error line on stderr", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("wsmiss");
  auto const got = dispatch(fx, {"workflow", "show", "nope"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  // Exactly one `error: ` prefix — the engine renderer supplies it, so this
  // travels as a rendered payload rather than a message body.
  CHECK(got.err == "error: workflow 'nope' not found\n");
  CHECK_FALSE(got.db_open);
}

// --- `workflow run` (task 6272) -------------------------------------------
//
// The spawn is REAL in every case below: `$PLANAR_EXECUTE_BIN` points at a
// stub script written into the fixture root, reached through the fixture's
// env map, so nothing here mutates the test process's environment and no
// real `planar-execute` need exist on the machine.
//
// Three independent witnesses, the shape the editflow cycle established:
//   1. argv       — the stub records `"$@"` to a witness file.
//   2. provenance — `k_run_sentinel` appears NOWHERE in `src/`, so its
//                   presence proves the stub actually ran.
//   3. exit code  — the stub exits with a code these cases chose.
// Break-probing `process::run_inherited`'s fork/execv kills every one of
// them by name; see src/lib/process/process.t.cpp's header.

TEST_CASE("workflow run on a miss exits 1 with the SAME line workflow show emits", "[cmd][handlers][parity]") {
  auto const fx = make_fixture("wrunmiss");
  // No PLANAR_EXECUTE_BIN is set: resolution must fail BEFORE any spawn is
  // attempted, so a missing workflow can never be reported as a spawn
  // failure.
  auto const got = dispatch(fx, {"workflow", "run", "nope", "--phase", "x"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  // Byte-identical to `workflow show nope`, single quotes included — one
  // renderer serving both leaves.
  CHECK(got.err == "error: workflow 'nope' not found\n");
  CHECK_FALSE(got.db_open);
}

TEST_CASE("workflow run execs planar-execute with the exact argv the oracle builds", "[cmd][handlers][spawn]") {
  auto fx = make_fixture("wrunargv");
  // The `@meta` name differs from the FILENAME on purpose: it proves the
  // spawn receives the resolved PATH, not the name the operator typed.
  write_shipped_workflow(fx, "finalize_closeout.lua",
                         "--[[ @meta\n"
                         "name: finalize-closeout\n"
                         "phases: closeout\n"
                         "--]]\n");

  auto const witness = fx.root / "argv.txt";
  auto const stub    = fx.root / "stub-execute.sh";
  write_run_stub(stub, witness, 0);
  fx.vars["PLANAR_EXECUTE_BIN"] = stub.string();

  auto const got = dispatch(fx, {"workflow", "run", "finalize-closeout", "--phase", "closeout", "--args", R"({"k":1})"});
  CHECK(got.code == 0);
  CHECK_FALSE(got.db_open);

  auto const recorded = slurp_witness(witness);
  // Non-emptiness FIRST: a witness that silently stayed empty would let
  // every assertion below pass vacuously.
  REQUIRE_FALSE(recorded.empty());
  CHECK(recorded.contains(k_run_sentinel));

  auto const workflow_path = (fx.root / "home" / "workflows" / "finalize_closeout.lua").string();
  // `run <resolved-path> --phase <phase> --args <json>` — the oracle's
  // order, and the resolved PATH rather than the name the operator typed.
  CHECK(recorded == std::format("{}\nrun\n{}\n--phase\ncloseout\n--args\n{{\"k\":1}}\n", k_run_sentinel, workflow_path));
}

TEST_CASE("workflow run omits an absent optional flag rather than passing it empty", "[cmd][handlers][spawn]") {
  auto fx = make_fixture("wrunopt");
  write_shipped_workflow(fx, "wf.lua", "-- @meta name: wf\n");

  auto const witness = fx.root / "argv.txt";
  auto const stub    = fx.root / "stub-execute.sh";
  write_run_stub(stub, witness, 0);
  fx.vars["PLANAR_EXECUTE_BIN"] = stub.string();

  auto const got = dispatch(fx, {"workflow", "run", "wf", "--phase", "build"});
  CHECK(got.code == 0);

  auto const recorded = slurp_witness(witness);
  REQUIRE_FALSE(recorded.empty());
  // The three optional flags are ABSENT, not present-and-empty: `--args ""`
  // is a different invocation from no `--args` at all.
  CHECK_FALSE(recorded.contains("--args"));
  CHECK_FALSE(recorded.contains("--worktree"));
  CHECK_FALSE(recorded.contains("--sandbox-root"));
  auto const workflow_path = (fx.root / "home" / "workflows" / "wf.lua").string();
  CHECK(recorded == std::format("{}\nrun\n{}\n--phase\nbuild\n", k_run_sentinel, workflow_path));
}

TEST_CASE("workflow run propagates planar-execute's exit code EXACTLY", "[cmd][handlers][spawn]") {
  auto fx = make_fixture("wrunexit");
  write_shipped_workflow(fx, "wf.lua", "-- @meta name: wf\n");

  auto const witness = fx.root / "argv.txt";
  auto const stub    = fx.root / "stub-execute.sh";
  // 23 is this case's own choice and matches no `domain_error_kind` bucket
  // — which is the point. A handler that routed the failure through the
  // bucket table would report 1 here.
  write_run_stub(stub, witness, 23);
  fx.vars["PLANAR_EXECUTE_BIN"] = stub.string();

  auto const got = dispatch(fx, {"workflow", "run", "wf", "--phase", "build"});
  CHECK(got.code == 23);
  // Nothing is added to either stream: the child already spoke in its own
  // voice, over the INHERITED descriptors this handler never captured.
  CHECK(got.out.empty());
  CHECK(got.err.empty());

  // The spawn really happened — otherwise "exit 23" could come from a
  // handler that never forked.
  auto const recorded = slurp_witness(witness);
  REQUIRE_FALSE(recorded.empty());
  CHECK(recorded.contains(k_run_sentinel));
}

TEST_CASE("workflow run reports an unspawnable planar-execute distinctly from a non-zero exit", "[cmd][handlers][spawn]") {
  auto fx = make_fixture("wrunnobin");
  write_shipped_workflow(fx, "wf.lua", "-- @meta name: wf\n");
  // An absolute path that does not exist. `resolve_program` refuses before
  // forking, so this is "never ran" rather than any exit status — the same
  // distinction `gh binary not on PATH` rests on.
  auto const absent             = (fx.root / "no-such-execute").string();
  fx.vars["PLANAR_EXECUTE_BIN"] = absent;

  auto const got = dispatch(fx, {"workflow", "run", "wf", "--phase", "build"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == std::format("error: spawning planar-execute: {}\n", absent));
}

TEST_CASE("annotate add without --anchor-path exits 2 before touching the engine", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("aareq");
  auto const got = dispatch(fx, {"annotate", "add"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: --anchor-path is required\n");
  // The refusal precedes scope resolution, so no database is opened. That
  // ordering is the Zig handler's and is worth pinning: reversing it would
  // migrate a database just to reject a malformed invocation.
  CHECK_FALSE(got.db_open);
}

TEST_CASE("annotate list on an empty database renders both wire formats", "[cmd][handlers][parity][terminator]") {
  auto const fx = make_fixture("alempty");

  auto const text = dispatch(fx, {"annotate", "list"});
  CHECK(text.code == 0);
  CHECK(text.out == "(no annotations)\n");
  CHECK(text.db_open);

  auto const json = dispatch(fx, {"annotate", "list", "--json"});
  CHECK(json.code == 0);
  // Terminated, where `workflow list --json` on an empty catalog is zero
  // bytes. Two leaves, two empty-output conventions, both correct — the
  // rule is each renderer's own @return, not a blanket policy.
  CHECK(json.out == "[]\n");
}

TEST_CASE("annotate add composes engine_identity and engine_planning end to end", "[cmd][handlers][parity][composition]") {
  // The layer-3 composition D20 exists for. `resolve_write_scope` reaches
  // engine_identity, `annotation::create` reaches engine_planning, and
  // those two buckets cannot reach each other — cmake/architecture.cmake
  // FATALs on the edge. This is only expressible here.
  auto const fx = make_fixture("aaadd");

  auto const added = dispatch(fx, {"annotate", "add", "--anchor-path", "src/foo.zig", "--line-start", "3", "--line-end", "7",
                                   "--title", "T1", "--body", "B1", "--tags", "a, b,a"});
  REQUIRE(added.code == 0);
  CHECK(added.err.empty());
  CHECK(added.db_open);
  CHECK(added.out.starts_with("id:          1\n"
                              "title:       T1\n"
                              "status:      active\n"
                              "scope:       global\n"
                              "anchor path: src/foo.zig\n"
                              "anchor line: 3-7\n"
                              // Note `a` appears once: --tags splits on ',',
                              // trims, and the engine de-duplicates.
                              "tags:        a, b\n"
                              "body:        B1\n"
                              "created:     "));

  auto const listed = dispatch(fx, {"annotate", "list"});
  CHECK(listed.code == 0);
  CHECK(listed.out == "    1  active      T1\n");
}

TEST_CASE("annotate add stores the cwd-derived association scope", "[cmd][handlers][parity][composition]") {
  // The composition test above passes even if `resolve_write_scope` were
  // deleted, because an unassociated cwd resolves to global anyway and
  // `scope: global` is also what an unresolved scope produces. This case
  // removes that ambiguity: with the cwd joined to an association, the
  // engine_identity half of the composition is the ONLY thing that can
  // produce `association:1`, so dropping it flips the assertion.
  //
  // Oracle fixture and capture (scratch PLANAR_DB/PLANAR_HOME/HOME, cwd =
  // the registered repo, `planar init` + `assoc create alpha --kind project`
  // + `assoc add alpha <cwd>`):
  //   $Z scope show          -> "resolved scope (from cwd):\n  project:alpha\n..."
  //   $Z annotate add --anchor-path src/baz.zig --title T3
  //     b'id:          3\ntitle:       T3\nstatus:      active\n
  //       scope:       association:1\nanchor path: src/baz.zig\n
  //       created:     <ts>\nupdated:     <ts>\n'
  //   $Z annotate add --anchor-path src/qux.zig --title T4 --json
  //     b'{"id":4,"scope_kind":"association","scope_id":1,...}\n'
  //
  // The association is seeded through engine_identity's own API rather than
  // raw SQL, so the fixture exercises the same code path `assoc create` /
  // `assoc add` would (`add_member` find-or-creates the project row).
  auto const fx = make_fixture("aascope");
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto const created =
        planar::engine::identity::create(**conn, {.slug = "alpha", .kind = planar::engine::identity::association_kind::project});
    REQUIRE(created.has_value());
    auto const joined = planar::engine::identity::add_member(**conn, "alpha", (fx.root / "proj").string());
    REQUIRE(joined.has_value());
  }

  auto const text = dispatch(fx, {"annotate", "add", "--anchor-path", "src/baz.zig", "--title", "T3"});
  REQUIRE(text.code == 0);
  CHECK(text.out.starts_with("id:          1\n"
                             "title:       T3\n"
                             "status:      active\n"
                             "scope:       association:1\n"
                             "anchor path: src/baz.zig\n"));

  auto const json = dispatch(fx, {"annotate", "add", "--anchor-path", "src/qux.zig", "--title", "T4", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.starts_with("{\"id\":2,\"scope_kind\":\"association\",\"scope_id\":1,"));
}

TEST_CASE("annotate add --json emits the oracle's object with a terminator appended", "[cmd][handlers][parity][terminator]") {
  auto const fx = make_fixture("aajson");
  auto const got =
      dispatch(fx, {"annotate", "add", "--anchor-path", "src/bar.zig", "--line-start", "1", "--title", "T2", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with("{\"id\":1,\"scope_kind\":\"global\",\"scope_id\":null,"
                            "\"anchor\":{\"path\":\"src/bar.zig\",\"line_start\":1,\"line_end\":null,"
                            "\"commit_sha\":\"\",\"text_hash\":\"\",\"text\":\"\"},"
                            "\"title\":\"T2\",\"slug\":null,\"body\":\"\",\"status\":\"active\","
                            "\"vendor\":\"\",\"plan_id\":null,\"task_id\":null,\"tags\":[],\"created_at\":\""));
  // render_json documents itself as returning NO trailing newline, so the
  // handler appends one. Exactly one.
  CHECK(got.out.ends_with("\"}\n"));
  CHECK(std::ranges::count(got.out, '\n') == 1);
}

TEST_CASE("annotate list rejects an unknown --status with exit 1", "[cmd][handlers][parity]") {
  // Exit 1, not 2: zig .../annotate/list.zig raises error.InvalidStatus,
  // which has no arm in exit.zig's codeFor and lands in the generic
  // bucket. The message reads like a user-input error and the code is not
  // one — which is exactly why it is pinned rather than inferred.
  auto const fx  = make_fixture("alstatus");
  auto const got = dispatch(fx, {"annotate", "list", "--status", "bogus"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: unknown status 'bogus'\n");
}

// =========================================================================
// Task 6106 — the leaves that were waiting for this layer.
// =========================================================================

// The parser itself now lives once, at layer 1 (`planar.cliapp.args`) —
// task 6123 collapsed the three copies the tree carried. This case stays
// HERE, in the operator binary's suite, because `planar unlink <arg>` is
// where every one of these values was captured against the oracle and
// where the behaviour is operator-visible.
TEST_CASE("parse_int64_zig reproduces std.fmt.parseInt, separators included", "[cmd][args][parity]") {
  using planar::cliapp::parse_int64_zig;

  // Every one of these was captured through `planar unlink <arg>` against
  // the oracle; the message interpolates the PARSED value, so a divergence
  // is operator-visible. See parse_int64_zig's doc comment for the table.
  CHECK(parse_int64_zig("12") == 12);
  CHECK(parse_int64_zig("+12") == 12);
  CHECK(parse_int64_zig("-5") == -5);
  CHECK(parse_int64_zig("007") == 7);
  CHECK(parse_int64_zig("0") == 0);
  // The case a `std::from_chars`-only port silently gets wrong.
  CHECK(parse_int64_zig("1_0") == 10);
  CHECK(parse_int64_zig("1__0") == 10);
  CHECK(parse_int64_zig("9223372036854775807") == 9223372036854775807LL);

  CHECK_FALSE(parse_int64_zig("abc").has_value());
  CHECK_FALSE(parse_int64_zig("12abc").has_value());
  CHECK_FALSE(parse_int64_zig(" 12").has_value());
  CHECK_FALSE(parse_int64_zig("12 ").has_value());
  CHECK_FALSE(parse_int64_zig("0x10").has_value());
  CHECK_FALSE(parse_int64_zig("_10").has_value());
  CHECK_FALSE(parse_int64_zig("10_").has_value());
  CHECK_FALSE(parse_int64_zig("+_1").has_value());
  CHECK_FALSE(parse_int64_zig("-_1").has_value());
  CHECK_FALSE(parse_int64_zig("++5").has_value());
  CHECK_FALSE(parse_int64_zig("").has_value());
  CHECK_FALSE(parse_int64_zig("+").has_value());
  CHECK_FALSE(parse_int64_zig("-").has_value());
  CHECK_FALSE(parse_int64_zig("_").has_value());
  // Exactly at the i64 boundary, which is where an unchecked accumulator
  // would wrap instead of refusing.
  CHECK_FALSE(parse_int64_zig("9223372036854775808").has_value());
  CHECK_FALSE(parse_int64_zig("99999999999999999999").has_value());
}

TEST_CASE("ext list --json OMITS a null base_url and default_project entirely", "[cmd][handlers][parity][terminator]") {
  // Not reachable through `ext register`: both register helpers always set
  // both columns, so this branch never fires under the CLI and a break-probe
  // that replaced it with `value_or("")` survived the whole parity suite. The
  // branch is still real — `system::register_system` takes both as optionals,
  // and a future `linear` / `gitlab-issues` registration need not set a base
  // URL — and the Zig renderer branches on the optional rather than
  // serializing it, so a `"base_url":null` would be a divergence the moment
  // such a row exists. Seeded here with raw SQL, which is the only way in —
  // and the SAME row was seeded into a scratch database and read back through
  // the ORACLE, so both expectations below are CAPTURED bytes:
  //
  //   $Z ext list --json
  //     b'{"id":1,"kind":"linear","slug":"lin","auth_method":"token-env",
  //       "created_at":"..."}\n'
  //   $Z ext list
  //     b'slug                  kind              base-url
  //       project\nlin                   linear
  //                       \n'
  //
  // Note the TEXT form's TRAILING WHITESPACE. The empty project column is
  // last and unpadded, but the empty base-url column before it is padded to
  // 36, so the line ends in spaces. Trimming it would be a divergence.
  auto const fx = make_fixture("extnull");
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    REQUIRE((*conn)
                ->execute("insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) "
                          "values ('linear', 'lin', null, null, 'token-env', 'TOK')")
                .has_value());
  }

  auto const got = dispatch(fx, {"ext", "list", "--json"});
  CHECK(got.code == 0);
  // Neither key appears at all, and the key ORDER around the gap is
  // unchanged: id, kind, slug, [base_url], [default_project], auth_method,
  // created_at.
  CHECK(got.out.find("base_url") == std::string::npos);
  CHECK(got.out.find("default_project") == std::string::npos);
  CHECK(got.out.find("null") == std::string::npos);
  CHECK(got.out.starts_with(R"({"id":1,"kind":"linear","slug":"lin","auth_method":"token-env","created_at":")"));
  CHECK(got.out.ends_with("\"}\n"));

  // And the TEXT renderer prints an empty cell rather than the word `null`,
  // padded to the same width.
  auto const text = dispatch(fx, {"ext", "list"});
  CHECK(text.code == 0);
  CHECK(text.out == "slug                  kind              base-url                              project\n"
                    "lin                   linear                                                  \n");
}

TEST_CASE("unlink on a missing id exits 1 naming the PARSED id", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("ulmiss");
  auto const got = dispatch(fx, {"unlink", "999"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: link 999 not found\n");
  CHECK(got.db_open);

  // `1_0` addresses link 10 on the reference binary, and the message says
  // so. This is the assertion that fails if the separator handling is
  // dropped — the plain "not found" path would still pass on `10`.
  auto const separated = dispatch(fx, {"unlink", "1_0"});
  CHECK(separated.code == 1);
  CHECK(separated.err == "error: link 10 not found\n");

  auto const padded = dispatch(fx, {"unlink", "007"});
  CHECK(padded.code == 1);
  CHECK(padded.err == "error: link 7 not found\n");
}

TEST_CASE("unlink on an unparseable id exits 2 AFTER opening the database", "[cmd][handlers][parity]") {
  // The ordering is the Zig handler's and it is observable from outside:
  // `unlink abc` leaves a created, migrated $PLANAR_DB behind. Validating
  // first would be tidier and would diverge, so `db_open` is asserted TRUE
  // here where `annotate add`'s equivalent refusal asserts it FALSE.
  auto const fx  = make_fixture("ulbad");
  auto const got = dispatch(fx, {"unlink", "abc"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: invalid link id 'abc'\n");
  CHECK(got.db_open);

  auto const lead = dispatch(fx, {"unlink", "_10"});
  CHECK(lead.code == 2);
  CHECK(lead.err == "error: invalid link id '_10'\n");
}

TEST_CASE("unlink composes engine_external and engine_runtime end to end", "[cmd][handlers][parity][composition]") {
  // The D20 composition this milestone was named for, and the second
  // instance of it in the binary. `link::remove` reaches engine_external,
  // `session::ensure_active` + `append_entry` reach engine_runtime, and
  // cmake/architecture.cmake FATALs on an edge between them — so the audit
  // row and the delete can only meet here.
  auto const fx = make_fixture("ulok");

  std::int64_t first  = 0;
  std::int64_t second = 0;
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());

    // `external_systems` has no engine entry point in this tree — the
    // system surface is not ported (see engine/external/CMakeLists.txt) —
    // so this one row is raw SQL. The links themselves go through the
    // engine.
    auto sys = (*conn)->prepare("insert into external_systems (kind, slug, default_project, auth_method, auth_ref) "
                                "values ('github-issues', 'gh', 'owner/repo', 'gh-cli', '') returning id");
    REQUIRE(sys.has_value());
    auto stepped = sys->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    auto const system_id = sys->column_int64(0);

    namespace link = planar::engine::external::link;
    auto a         = link::create(
        **conn, {.entity_kind = link::external_entity_kind::plan, .entity_id = 1, .system_id = system_id, .external_id = "42"});
    REQUIRE(a.has_value());
    first  = a->id;
    auto b = link::create(
        **conn, {.entity_kind = link::external_entity_kind::plan, .entity_id = 2, .system_id = system_id, .external_id = "43"});
    REQUIRE(b.has_value());
    second = b->id;
  }

  auto const text = dispatch(fx, {"unlink", std::to_string(first)});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  CHECK(text.out == std::format("unlinked: external link {} removed\n", first));

  auto const json = dispatch(fx, {"unlink", std::to_string(second), "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == std::format("{{\"ok\":true,\"id\":{}}}\n", second));

  // Gone, and a second attempt is the not-found path.
  auto const again = dispatch(fx, {"unlink", std::to_string(first)});
  CHECK(again.code == 1);
  CHECK(again.err == std::format("error: link {} not found\n", first));

  // The engine_runtime half. There was no session before this test ran, so
  // `ensure_active` created one — and BOTH unlinks appended to it, in
  // order. Asserting only "some row exists" would pass with the append
  // removed from one branch.
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto stmt = (*conn)->prepare("select prefix, body from session_entries order by session_id, ordinal");
    REQUIRE(stmt.has_value());

    std::vector<std::pair<std::string, std::string>> rows;
    for (;;) {
      auto stepped = stmt->step();
      REQUIRE(stepped.has_value());
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      rows.emplace_back(stmt->column_text(0), stmt->column_text(1));
    }
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].first == "action");
    CHECK(rows[0].second == std::format("unlink: removed external link {}", first));
    CHECK(rows[1].second == std::format("unlink: removed external link {}", second));
  }
}

TEST_CASE("unlink attributes the audit row to $PLANAR_VENDOR", "[cmd][handlers][composition]") {
  // The vendor comes off the CONTEXT's env-lookup, not std::getenv — which
  // is what lets this case exist at all. It is also the assertion that
  // fails if the handler is "simplified" to call
  // `session::vendor_from_env()`: that function reads the real process
  // environment, where PLANAR_VENDOR is unset, so the session would be
  // attributed to `cli` regardless of the fixture.
  auto fx = make_fixture("ulvendor");
  fx.vars.emplace("PLANAR_VENDOR", "claude");
  fx.vars.emplace("PLANAR_VENDOR_SESSION_ID", "abc123");

  auto const got = dispatch(fx, {"unlink", "999"});
  REQUIRE(got.code == 1); // no link, but the refusal path writes no session either

  // Now a real one, so a session is actually opened.
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto sys = (*conn)->prepare("insert into external_systems (kind, slug, default_project, auth_method, auth_ref) "
                                "values ('github-issues', 'gh', 'owner/repo', 'gh-cli', '') returning id");
    REQUIRE(sys.has_value());
    auto stepped = sys->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    namespace link = planar::engine::external::link;
    REQUIRE(link::create(**conn, {.entity_kind = link::external_entity_kind::task,
                                  .entity_id   = 7,
                                  .system_id   = sys->column_int64(0),
                                  .external_id = "77"})
                .has_value());
  }
  REQUIRE(dispatch(fx, {"unlink", "1"}).code == 0);

  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto stmt = (*conn)->prepare("select vendor, vendor_session_id from sessions");
    REQUIRE(stmt.has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_text(0) == "claude");
    CHECK(stmt->column_text(1) == "abc123");
  }
}

TEST_CASE("skills renders its own help page and exits 0 without a database", "[cmd][handlers][parity]") {
  // A childless node is a LEAF here (`cliapp::leaf_keys`), so without a handler
  // this verb would answer `error: not implemented yet` and exit 64. The
  // oracle answers exit 0 and a help page; these bytes are that page.
  auto const fx  = make_fixture("skills");
  auto const got = dispatch(fx, {"skills"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);
  // Re-baselined onto CLI11's formatter by task 6123 and pinned exactly.
  // The PROSE is still the oracle's, verbatim — that text is the operator's
  // only pointer to scriptorium, and losing it would silently strand
  // anyone who runs the retired verb.
  CHECK(got.out == "The unified skill source tree under skills/src/ is rendered by the\n"
                   "external scriptorium binary (plan 918). Planar no longer renders vendor\n"
                   "projections nor tracks their install-drift in-band; use `scriptorium\n"
                   "check`/`scriptorium status` instead. This command has no subcommands.\n"
                   "\n"
                   "\n"
                   "skills [OPTIONS]\n"
                   "\n"
                   "\n"
                   "OPTIONS:\n"
                   "  -h,     --help              Print this help message and exit\n");

  // `planar skills --help` renders the SAME page through dispatch's help
  // path rather than the handler. Both routes must agree — if they did not,
  // the handler would be rendering something the tree does not say.
  auto const helped = dispatch(fx, {"skills", "--help"});
  CHECK(helped.code == 0);
  CHECK(helped.out == got.out);
}

TEST_CASE("skills rejects a positional the way a flagless leaf must", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("skillsx");
  auto const got = dispatch(fx, {"skills", "extra"});
  // The EXIT CODE is the contract and did not move: 2 on the operator
  // binary, where planar-agent and planar-watch would say 1. Task 6123
  // re-baselined the wording — CLI11 reports a rejected extra token as
  // `ExtrasError` where etcli distinguished `TooManyPositionals` — and the
  // new bytes are pinned exactly.
  CHECK(got.code == 2);
  CHECK(got.out == "error: skills: The following argument was not expected: extra\n");
  CHECK(got.err == "error: ExtrasError\n");
}

TEST_CASE("workspace doctor on an empty database: zero bytes vs {\"orgs\":[]}", "[cmd][handlers][parity][terminator]") {
  // The two render modes of ONE leaf disagreeing on empty. Both renderers
  // return COMPLETE payloads and the handler appends to neither; a blanket
  // append would put a stray newline on the text path.
  auto const fx = make_fixture("wdempty");

  auto const text = dispatch(fx, {"workspace", "doctor"});
  CHECK(text.code == 0);
  CHECK(text.out.empty());
  CHECK(text.err.empty());
  CHECK(text.db_open);

  auto const json = dispatch(fx, {"workspace", "doctor", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "{\"orgs\":[]}\n");
}

TEST_CASE("workspace doctor diagnoses and repairs a registered org", "[cmd][handlers][parity]") {
  // Doctor is NOT read-only: it creates the state directory as a side
  // effect of being asked what is wrong, and it writes to the root recorded
  // in the association's config_json — not to the cwd. Both are asserted,
  // because both are the reason this leaf is only safe against a scratch
  // $PLANAR_DB.
  auto const fx        = make_fixture("wdorg");
  auto const root_path = (fx.root / "proj").string();
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto const created =
        planar::engine::identity::create(**conn, {.slug        = "acme",
                                                  .kind        = planar::engine::identity::association_kind::org,
                                                  .config_json = std::format(R"({{"root_path":"{}"}})", root_path)});
    REQUIRE(created.has_value());
  }

  auto const state_dir = fx.root / "home" / "workspaces" / "1";
  REQUIRE_FALSE(std::filesystem::exists(state_dir));

  auto const first = dispatch(fx, {"workspace", "doctor"});
  CHECK(first.code == 0);
  CHECK(first.err.empty());
  // Five entries, and the shape is worth reading: the AGENTS.md target is
  // reported `missing` and symlinked TO in the SAME pass, so the links
  // dangle until `workspace regenerate` writes it. That is the oracle's
  // behavior, captured against this exact fixture (an org association with
  // a `root_path` and nothing on disk yet), not an artifact of the port.
  // Note both links point at AGENTS.md — CLAUDE.md is an alias, not a
  // second target.
  CHECK(first.out == std::format("fix: created state dir {0}\n"
                                 "missing: {0}/AGENTS.md (run `planar workspace regenerate` after M3)\n"
                                 "missing: {0}/routing-table.json (run `planar workspace regenerate` after M3)\n"
                                 "fix: reinstalled symlink {1}/AGENTS.md \xe2\x86\x92 {0}/AGENTS.md\n"
                                 "fix: reinstalled symlink {1}/CLAUDE.md \xe2\x86\x92 {0}/AGENTS.md\n"
                                 "org:acme repaired 5 issues\n",
                                 state_dir.string(), root_path));
  // The repair actually happened, in $PLANAR_HOME — not the cwd.
  CHECK(std::filesystem::exists(state_dir));
  // And the root guidance links landed at the DATABASE's root_path.
  CHECK(std::filesystem::is_symlink(fx.root / "proj" / "AGENTS.md"));
  CHECK(std::filesystem::is_symlink(fx.root / "proj" / "CLAUDE.md"));

  // Second pass: the state dir now exists, so the `fix` line is gone and
  // `issues_found` drops. That the count MOVES is what proves the first
  // pass repaired rather than merely reported.
  auto const second = dispatch(fx, {"workspace", "doctor", "--json"});
  CHECK(second.code == 0);
  CHECK(second.out == std::format("{{\"orgs\":[{{\"slug\":\"acme\",\"org_id\":1,\"issues_found\":2,"
                                  "\"issues_repaired\":[{{\"kind\":\"missing\",\"detail\":"
                                  "\"{}/AGENTS.md (run `planar workspace regenerate` after M3)\"}},"
                                  "{{\"kind\":\"missing\",\"detail\":"
                                  "\"{}/routing-table.json (run `planar workspace regenerate` after M3)\"}}]}}]}}\n",
                                  state_dir.string(), state_dir.string()));
}

TEST_CASE("workspace doctor reports an unreadable config without repairing the root", "[cmd][handlers][parity]") {
  // `uncertain` is the shape that both skips the repair AND says why. A
  // handler that dropped the error entry would look identical on the happy
  // path.
  auto const fx = make_fixture("wdbad");
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    REQUIRE(planar::engine::identity::create(
                **conn,
                {.slug = "acme", .kind = planar::engine::identity::association_kind::org, .config_json = std::string{"{oops"}})
                .has_value());
  }

  auto const got = dispatch(fx, {"workspace", "doctor"});
  CHECK(got.code == 0);
  // Four, not five: the two `missing` targets and the created state dir are
  // still reported, but the two symlink `fix` lines the readable-config
  // fixture produces are ABSENT — the repair was skipped, which is the
  // whole point of `uncertain`.
  CHECK(got.out.ends_with("error: workspace config_json is malformed; skipping root guidance repair\n"
                          "org:acme repaired 4 issues\n"));
  CHECK(std::ranges::count(got.out, '\n') == 5);
  // No root guidance was installed, because the shape could not be read.
  CHECK_FALSE(std::filesystem::exists(fx.root / "proj" / "AGENTS.md"));
}

TEST_CASE("workspace init creates durable org membership routing and root guidance", "[cmd][handlers][workspace-init]") {
  // A non-empty child fixture is essential: the oracle refuses before it
  // writes when the scan finds no nested checkout, so an empty fixture cannot
  // falsify a handler that quietly skips discovery.
  auto const      fx = make_fixture("wsinit_fresh");
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "proj" / "alpha" / ".git", ec);
  REQUIRE_FALSE(ec);

  auto const got = dispatch(fx, {"workspace", "init", "--name", "Acme", "--slug", "acme"});
  INFO(got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out.contains("created org:acme (Acme)\n"));
  CHECK(got.out.contains("project:alpha"));
  CHECK(got.out.contains("(auto-created, member-of org:acme)"));
  CHECK(got.out.contains("1 repos initialized as projects, all members of org:acme.\n"));
  CHECK(std::filesystem::exists(fx.root / "home" / "workspaces" / "1" / "routing-table.json"));
  CHECK(std::filesystem::exists(fx.root / "home" / "workspaces" / "1" / "AGENTS.md"));
  CHECK(std::filesystem::is_symlink(fx.root / "proj" / "AGENTS.md"));
  CHECK(std::filesystem::is_symlink(fx.root / "proj" / "CLAUDE.md"));

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto               conn = ctx.ensure_db();
  REQUIRE(conn.has_value());
  auto org_count = (*conn)->prepare("select count(*) from associations where slug='acme' and kind='org'");
  REQUIRE(org_count.has_value());
  REQUIRE(org_count->step().has_value());
  CHECK(org_count->column_int64(0) == 1);
  auto member_count = (*conn)->prepare("select count(*) from project_associations");
  REQUIRE(member_count.has_value());
  REQUIRE(member_count->step().has_value());
  CHECK(member_count->column_int64(0) == 1);

  // Reuse must not duplicate either durable row. The fresh fixture above has
  // a real member, so this arm distinguishes idempotence from a no-op path.
  auto const again = dispatch(fx, {"workspace", "init", "--name", "Acme", "--slug", "acme", "--json"});
  CHECK(again.code == 0);
  CHECK(again.err.empty());
  CHECK(again.out.contains("\"org\":{\"id\":1,\"slug\":\"acme\",\"name\":\"Acme\",\"created\":false}"));
  CHECK(again.out.contains("\"projects\":[{\"slug\":\"alpha\""));
  CHECK(again.out.contains("\"created\":false,\"membership_created\":false}"));
  CHECK(
      again.out.contains("\"routing\":{\"project_count\":1,\"cross_repo_deps\":0,\"enrich_enabled\":false,\"enrich_misses\":0}"));
  CHECK(again.out.contains("\"symlinks\":{\"strategy\":\"symlink\",\"installed\":[\"AGENTS.md\",\"CLAUDE.md\"]}"));
  auto member_count_after = (*conn)->prepare("select count(*) from project_associations");
  REQUIRE(member_count_after.has_value());
  REQUIRE(member_count_after->step().has_value());
  CHECK(member_count_after->column_int64(0) == 1);
}

TEST_CASE("workspace init rejects incompatible flags before opening or mutating the database",
          "[cmd][handlers][workspace-init][refusal]") {
  auto const fx  = make_fixture("wsinit_refusal");
  auto const got = dispatch(fx, {"workspace", "init", "--no-scan", "--enrich"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: cannot combine --no-scan and --enrich\n");
  CHECK_FALSE(got.db_open);
  CHECK_FALSE(std::filesystem::exists(fx.db_path));
  CHECK_FALSE(std::filesystem::exists(fx.root / "home" / "workspaces"));
}

TEST_CASE("workspace init meta-repo refuses a reused org recorded at another root without linking either repository",
          "[cmd][handlers][workspace-init][meta][refusal]") {
  auto const      fx = make_fixture("wsinit_meta_root");
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "proj" / ".git", ec);
  std::filesystem::create_directories(fx.root / "proj" / "alpha" / ".git", ec);
  REQUIRE_FALSE(ec);

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto               conn = ctx.ensure_db();
  REQUIRE(conn.has_value());
  auto const other_root = (fx.root / "elsewhere").string();
  REQUIRE(planar::engine::identity::create(
              **conn, {.slug        = "acme",
                       .name        = "Acme",
                       .kind        = planar::engine::identity::association_kind::org,
                       .config_json = std::format(R"({{"root_path":"{}","workspace_shape":"meta-repo"}})", other_root)})
              .has_value());

  auto const got = dispatch(fx, {"workspace", "init", "--meta-repo", "--name", "Acme", "--slug", "acme"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: org:acme already exists with a different workspace root; choose a different --slug or run from the "
                   "recorded root\n");
  auto links = (*conn)->prepare("select count(*) from project_associations");
  REQUIRE(links.has_value());
  REQUIRE(links->step().has_value());
  CHECK(links->column_int64(0) == 0);
}

// =========================================================================
// task 6110 — `workspace routing show`.
//
// The decoder and both renderers are unit-tested in
// src/lib/engine/workspace/routing.t.cpp. What only THIS layer can cover is
// the path resolution, the file read, and the mapping of four failures onto
// three exit codes — including the two decode failures that share one
// message template and differ only in exit code.
//
// ORACLE PROVENANCE. Captured under a scratch HOME / PLANAR_HOME /
// PLANAR_DB, an org registered via `workspace init --no-scan --name Acme
// --slug acme`, and routing-table.json overwritten per probe. Both streams
// through a pipe, never a file redirect.
//
//   no orgs at all
//     $Z workspace routing show --json   exit 1, stderr
//       b'error: no org associations registered; create one with `planar
//         workspace init`\n'
//     (`routing build` and `regenerate` emit the SAME line and exit 1.)
//
//   org present, routing-table.json absent
//     $Z workspace routing show --json   exit 1, stderr
//       b'error: routing table not found at <PH>/workspaces/1/
//         routing-table.json; run `planar workspace routing build` first\n'
//     Note this arrives on the --json arm too: the file read precedes the
//     arm split.
//
//   file = b'this is not json'
//     $Z workspace routing show --json   exit 0, stdout b'this is not json\n'
//     $Z workspace routing show          exit 1, stderr
//       b'error: decoding routing table failed: SyntaxError\n'
//
//   file = valid JSON with `schema_version` removed
//     $Z workspace routing show          exit 2, stderr
//       b'error: decoding routing table failed: InvalidInput\n'
// =========================================================================

namespace {

/// @brief Register one org and return its state directory.
///
/// Mirrors `workspace init`'s database effect without invoking it — `init`
/// is unported (layer 3), and this leaf only needs the association row.
auto seed_org(const fixture& fx) -> std::filesystem::path {
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto               conn = ctx.ensure_db();
  REQUIRE(conn.has_value());
  auto const created = planar::engine::identity::create(
      **conn, {.slug        = "acme",
               .kind        = planar::engine::identity::association_kind::org,
               .config_json = std::format(R"({{"root_path":"{}"}})", (fx.root / "proj").string())});
  REQUIRE(created.has_value());
  return fx.root / "home" / "workspaces" / "1";
}

/// @brief Write `body` to the org's routing table, creating the state dir.
auto write_routing_table(const std::filesystem::path& state_dir, std::string_view body) -> std::filesystem::path {
  std::filesystem::create_directories(state_dir);
  auto const    path = state_dir / "routing-table.json";
  std::ofstream file(path, std::ios::binary);
  file << body;
  file.close();
  return path;
}

} // namespace

TEST_CASE("workspace routing show refuses when no org is registered", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("wrsnoorg");
  auto const got = dispatch(fx, {"workspace", "routing", "show", "--json"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: no org associations registered; create one with `planar workspace init`\n");
}

TEST_CASE("workspace routing show refuses a missing table, naming the path", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrsmissing");
  auto const state_dir = seed_org(fx);
  auto const expected  = (state_dir / "routing-table.json").string();

  auto const got = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == std::format("error: routing table not found at {}; run `planar workspace routing build` first\n", expected));

  // The refusal reaches the --json arm too: the read precedes the split.
  auto const as_json = dispatch(fx, {"workspace", "routing", "show", "--json"});
  CHECK(as_json.code == 1);
  CHECK(as_json.err == got.err);

  // And `show` did NOT create the state directory on the way past — it uses
  // load_layout, not ensure_layout. This is the assertion that separates it
  // from `doctor`, which creates the directory as a side effect of being
  // asked what is wrong.
  CHECK_FALSE(std::filesystem::exists(state_dir));
}

TEST_CASE("workspace routing show json arm emits non-JSON bytes verbatim", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrsverbatim");
  auto const state_dir = seed_org(fx);
  write_routing_table(state_dir, "this is not json");

  auto const as_json = dispatch(fx, {"workspace", "routing", "show", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.err.empty());
  CHECK(as_json.out == "this is not json\n");

  // The SAME bytes on the text arm are a decode failure at exit 1. The two
  // arms disagreeing on identical input is the contract, not a defect.
  auto const as_text = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(as_text.code == 1);
  CHECK(as_text.out.empty());
  CHECK(as_text.err == "error: decoding routing table failed: SyntaxError\n");
}

TEST_CASE("workspace routing show maps its two decode failures onto DIFFERENT exit codes", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrsexit");
  auto const state_dir = seed_org(fx);

  // Malformed bytes -> SyntaxError, exit 1.
  write_routing_table(state_dir, "{not json");
  auto const syntax = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(syntax.code == 1);
  CHECK(syntax.err == "error: decoding routing table failed: SyntaxError\n");

  // Well-formed JSON missing a required field -> InvalidInput, exit 2.
  write_routing_table(state_dir, R"({"workspace_id":1,"workspace_slug":"a","workspace_name":"A",)"
                                 R"("generated_at":"T","projects":[],)"
                                 R"("cross_repo":{"dependency_edges":[]}})");
  auto const invalid = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(invalid.code == 2);
  CHECK(invalid.err == "error: decoding routing table failed: InvalidInput\n");

  // The exit codes DIFFER while the message differs only in the tag. A port
  // that folded these together would keep both stderr payloads plausible and
  // silently move one exit code.
  CHECK(syntax.code != invalid.code);
}

TEST_CASE("workspace routing show renders a decoded table", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrsrender");
  auto const state_dir = seed_org(fx);
  write_routing_table(state_dir, R"({"schema_version":1,"workspace_id":1,"workspace_slug":"acme","workspace_name":"Acme",)"
                                 R"("generated_at":"2020-01-01T00:00:00Z","generator_version":"gv-9","projects":[)"
                                 R"({"slug":"alpha","root_path":"/r/alpha","summary":"Alpha repo.","summary_source":"readme",)"
                                 R"("capabilities":["go-service"],"depends_on":["beta"],"depends_on_source":"go.mod",)"
                                 R"("planar_focus":{"open_tasks":4,"open_questions":5}}],)"
                                 R"("cross_repo":{"dependency_edges":[)"
                                 R"({"from":"alpha","to":"beta","reason":"go.mod replace"}]}})");

  auto const got = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == "workspace: org:acme (id 1)\n"
                   "generated: 2020-01-01T00:00:00Z (gv-9)\n"
                   "projects:  1\n"
                   "\n"
                   "- alpha\n"
                   "    path:         /r/alpha\n"
                   "    capabilities: go-service\n"
                   "    summary:      Alpha repo.\n"
                   "    depends_on:   beta\n"
                   "    open tasks:   4\n"
                   "    open Qs:      5\n"
                   "\n"
                   "cross-repo edges:\n"
                   "  alpha -> beta (go.mod replace)\n");

  // The --json arm on the SAME file returns the stored bytes, not this
  // render — proving the two arms read one file through two paths.
  auto const as_json = dispatch(fx, {"workspace", "routing", "show", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.out.starts_with(R"({"schema_version":1,"workspace_id":1)"));
  CHECK(as_json.out.ends_with("\n"));
  CHECK(as_json.out != got.out);
}

// =========================================================================
// task 6275 — `workspace routing build`, and one CORRECTION to `show`.
//
// ORACLE PROVENANCE. Captured against zig/zig-out/bin/planar in a pinned
// scratch arena (HOME / PLANAR_HOME / PLANAR_DB all redirected into it),
// BOTH streams through a pipe, exit code read OUTSIDE the pipe:
//
//   $Z workspace routing build
//     exit 0, stdout b'built <path> (3 projects, 2 cross-repo deps)\n'
//   $Z workspace routing build --json
//     exit 0, stdout b'{"path":"<path>","projects":3,"dependency_edges":2,
//                       "enrich_enabled":false,"enrich_misses":0}\n'
//   $Z workspace routing build --enrich
//     exit 0, stdout b'warning: --enrich is not yet implemented in Zig;
//                       skipping enrichment pass\nbuilt <path> (...)\n'
//     -- the warning is on STDOUT, and it PRECEDES the result line
//   $Z workspace routing build --enrich --json
//     exit 0, the JSON only; the warning is suppressed under --json
//   $Z workspace routing build nosuch          exit 1  no org associations ...
//   two orgs: $Z workspace routing build       exit 2  multiple org ...
//   malformed rules TOML                       exit 1  ... ParseFailed
//   unquoted rules scalar                      exit 2  ... InvalidInput
//   overrides = b'not json'                    exit 1  ... SyntaxError
//   overrides = b'[]'                          exit 2  ... InvalidInput
//
// AND the correction, captured side by side with two orgs registered:
//   zig  workspace routing show   exit 2
//   this workspace routing show   exit 1   <-- was wrong, see workspace.cppm
// with stderr byte-identical on both, which is why only an exit-code
// assertion could see it.

namespace {

/// @brief Register a second org, to reach the AMBIGUOUS refusal.
auto seed_second_org(const fixture& fx) -> void {
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto               conn = ctx.ensure_db();
  REQUIRE(conn.has_value());
  REQUIRE(planar::engine::identity::create(**conn, {.slug = "other", .kind = planar::engine::identity::association_kind::org})
              .has_value());
}

} // namespace

TEST_CASE("workspace routing build writes a table and reports it", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrbbuild");
  auto const state_dir = seed_org(fx);
  auto const expected  = (state_dir / "routing-table.json").string();

  // The state directory does NOT exist yet — `build` uses `ensure_layout`
  // and creates it, which is the difference from `show` beside it.
  CHECK_FALSE(std::filesystem::exists(state_dir));

  auto const got = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // Zero members, so zero projects and zero edges — the fixture registers an
  // org and no repositories.
  CHECK(got.out == std::format("built {} (0 projects, 0 cross-repo deps)\n", expected));
  CHECK(std::filesystem::exists(expected));
}

// =========================================================================
// task 6364 — `workspace regenerate`.
//
// The engine-level manifest suite pins xxh64 vectors and an oracle-captured
// document digest. These command-layer cases cover the three refusal guards
// and the full write path, including the two generated files. A hermetic
// live Zig/C++ differential run (same database seed and same absolute state
// path, resetting the arena between binaries) is recorded in the task report;
// the assertions below keep each independently reachable after that capture.
// =========================================================================

TEST_CASE("workspace regenerate refuses its missing-org, ambiguous-org, and missing-routing-table guards",
          "[cmd][handlers][parity]") {
  constexpr std::string_view k_no_org = "error: no org associations registered; create one with `planar workspace init`\n";
  constexpr std::string_view k_ambiguous =
      "error: multiple org associations registered; pass the workspace slug or id explicitly\n";

  // resolve_org's NotFound arm: no association exists at all.
  auto const empty = make_fixture("wrgnone");
  auto const none  = dispatch(empty, {"workspace", "regenerate", "--json"});
  CHECK(none.code == 1);
  CHECK(none.out.empty());
  CHECK(none.err == k_no_org);

  // The leaf gets past resolve_org before checking for the table; this is a
  // distinct NotFound guard and has a deliberately different message.
  auto const missing  = make_fixture("wrgmissing");
  auto const state    = seed_org(missing);
  auto const no_table = dispatch(missing, {"workspace", "regenerate"});
  CHECK(no_table.code == 1);
  CHECK(no_table.out.empty());
  CHECK(no_table.err == "error: routing table not found; run `planar workspace routing build` first\n");
  CHECK_FALSE(std::filesystem::exists(state / "AGENTS.md"));

  // resolve_org's InvalidInput arm: an unnamed selection among two orgs.
  auto const many = make_fixture("wrgmany");
  seed_org(many);
  seed_second_org(many);
  auto const ambiguous = dispatch(many, {"workspace", "regenerate"});
  CHECK(ambiguous.code == 2);
  CHECK(ambiguous.out.empty());
  CHECK(ambiguous.err == k_ambiguous);
}

TEST_CASE("workspace regenerate writes AGENTS.md and its xxh64 manifest", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrghappy");
  auto const state_dir = seed_org(fx);
  write_routing_table(state_dir, R"({"schema_version":1,"workspace_id":1,"workspace_slug":"acme","workspace_name":"Acme",)"
                                 R"("generated_at":"2026-08-31T00:00:00Z","generator_version":"test","projects":[],)"
                                 R"("cross_repo":{"dependency_edges":[]}})");

  auto const text = dispatch(fx, {"workspace", "regenerate"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());

  auto const agents_path   = state_dir / "AGENTS.md";
  auto const manifest_path = state_dir / ".manifest-docs";
  REQUIRE(std::filesystem::exists(agents_path));
  REQUIRE(std::filesystem::exists(manifest_path));

  std::ifstream     agents_input(agents_path, std::ios::binary);
  const std::string agents{std::istreambuf_iterator<char>(agents_input), std::istreambuf_iterator<char>()};
  CHECK(text.out == std::format("regenerated AGENTS.md for org:acme (0 projects, {} bytes)\n", agents.size()));
  CHECK(agents.contains("# Workspace AGENTS Guide — Acme\n"));
  // The inert `-}}` trim marker remains observable as the two blank lines
  // before this fallback paragraph — D2 requires retaining that quirk.
  CHECK(agents.contains("## Projects in this workspace\n\n\n_No projects registered."));

  std::ifstream     manifest_input(manifest_path, std::ios::binary);
  const std::string manifest{std::istreambuf_iterator<char>(manifest_input), std::istreambuf_iterator<char>()};
  CHECK(manifest.starts_with("{\"version\":1,\"algo\":\"xxh64\","));
  CHECK(manifest.contains("\"generated_at\":\"now\""));
  CHECK(manifest.contains(std::format("\"{}\"", agents_path.string())));

  auto const as_json = dispatch(fx, {"workspace", "regenerate", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.err.empty());
  CHECK(as_json.out.starts_with(std::format("{{\"agents_path\":\"{}\"", agents_path.string())));
  CHECK(as_json.out.contains(std::format("\"manifest_path\":\"{}\"", manifest_path.string())));
  CHECK(as_json.out.contains(std::format("\"project_count\":0,\"bytes_written\":{},", agents.size())));
  CHECK(as_json.out.ends_with("}\n"));
}

TEST_CASE("workspace routing build --json reports the hardcoded enrich fields", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrbjson");
  auto const state_dir = seed_org(fx);
  auto const expected  = (state_dir / "routing-table.json").string();

  auto const got = dispatch(fx, {"workspace", "routing", "build", "--json"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // `enrich_enabled` and `enrich_misses` are hardcoded in the oracle too —
  // the enrichment pass does not exist in either implementation.
  CHECK(got.out == std::format(R"({{"path":"{}","projects":0,"dependency_edges":0,)"
                               R"("enrich_enabled":false,"enrich_misses":0}})"
                               "\n",
                               expected));
}

TEST_CASE("workspace routing build --enrich warns on STDOUT, before the result, and only without --json",
          "[cmd][handlers][parity]") {
  // Three separate captures, because none of the three follows from the
  // others: the stream, the ORDER, and the --json suppression.
  auto const fx        = make_fixture("wrbenrich");
  auto const state_dir = seed_org(fx);
  auto const expected  = (state_dir / "routing-table.json").string();

  auto const text = dispatch(fx, {"workspace", "routing", "build", "--enrich"});
  CHECK(text.code == 0);
  // STDOUT, not stderr — stderr stays empty.
  CHECK(text.err.empty());
  CHECK(text.out == std::format("warning: --enrich is not yet implemented in Zig; skipping enrichment pass\n"
                                "built {} (0 projects, 0 cross-repo deps)\n",
                                expected));

  auto const as_json = dispatch(fx, {"workspace", "routing", "build", "--enrich", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.err.empty());
  // Suppressed entirely under --json, and the enrich fields stay false/0
  // even though the flag was passed.
  CHECK_FALSE(as_json.out.contains("warning"));
  CHECK(as_json.out.contains(R"("enrich_enabled":false,"enrich_misses":0)"));
}

TEST_CASE("workspace routing build refuses when no org is registered", "[cmd][handlers][parity]") {
  auto const fx  = make_fixture("wrbnoorg");
  auto const got = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: no org associations registered; create one with `planar workspace init`\n");
}

TEST_CASE("the AMBIGUOUS org refusal exits 2 on BOTH routing leaves", "[cmd][handlers][parity]") {
  // The task-6275 correction. `show` answered 1 here and 2 in the oracle,
  // with byte-identical stderr, so nothing that asserts on the MESSAGE could
  // catch it — this asserts the code. `build` is checked beside it because
  // both leaves refuse through the same `error.InvalidInput`.
  auto const fx = make_fixture("wrbambig");
  seed_org(fx);
  seed_second_org(fx);

  constexpr std::string_view k_message =
      "error: multiple org associations registered; pass the workspace slug or id explicitly\n";

  auto const built = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(built.code == 2);
  CHECK(built.err == k_message);

  auto const shown = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(shown.code == 2);
  CHECK(shown.err == k_message);

  // Discrimination: naming one of the two orgs resolves it, so exit 2 above
  // is about the ambiguity and not about having two rows in the table.
  auto const named = dispatch(fx, {"workspace", "routing", "build", "acme"});
  CHECK(named.code == 0);
  CHECK(named.err.empty());
}

TEST_CASE("workspace routing build splits its rules-file failures across two exit codes", "[cmd][handlers][parity]") {
  auto const fx = make_fixture("wrbrules");
  seed_org(fx);
  auto const rules = fx.root / "home" / "templates" / "workspace-capabilities.toml";
  std::filesystem::create_directories(rules.parent_path());

  auto const put_rules = [&](std::string_view body) {
    std::ofstream out(rules, std::ios::binary | std::ios::trunc);
    out << body;
  };

  put_rules("[[rule]]\ntag = \"x\"\nmatch_all = [oops]\n");
  auto const parse_failed = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(parse_failed.code == 1);
  CHECK(parse_failed.err == "error: loading capability rules failed: ParseFailed\n");

  put_rules("[[rule]]\ntag = unquoted\n");
  auto const invalid = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(invalid.code == 2);
  CHECK(invalid.err == "error: loading capability rules failed: InvalidInput\n");

  // A WELL-FORMED file is accepted, so the two refusals above are about the
  // content and not about the file's mere presence.
  put_rules("[[rule]]\ntag = \"ok\"\nmatch_all = [\"*.go\"]\n");
  auto const accepted = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(accepted.code == 0);
  CHECK(accepted.err.empty());
}

TEST_CASE("workspace routing build splits its overrides failures across two exit codes", "[cmd][handlers][parity]") {
  auto const fx        = make_fixture("wrbov");
  auto const state_dir = seed_org(fx);
  std::filesystem::create_directories(state_dir);
  auto const overrides = state_dir / "routing-table-overrides.json";

  auto const put_overrides = [&](std::string_view body) {
    std::ofstream out(overrides, std::ios::binary | std::ios::trunc);
    out << body;
  };

  put_overrides("not json\n");
  auto const syntax = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(syntax.code == 1);
  CHECK(syntax.err == "error: loading routing overrides failed: SyntaxError\n");

  // Parses, but is not an object — a DIFFERENT exit code behind the SAME
  // message template.
  put_overrides("[]\n");
  auto const invalid = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(invalid.code == 2);
  CHECK(invalid.err == "error: loading routing overrides failed: InvalidInput\n");

  put_overrides(R"({"schema_version":1,"projects":{}})");
  auto const accepted = dispatch(fx, {"workspace", "routing", "build"});
  CHECK(accepted.code == 0);
  CHECK(accepted.err.empty());
}

TEST_CASE("build then show is a round trip through the CLI", "[cmd][handlers][parity]") {
  // What this cycle exists for. Task 6110 could only SHOW a table some other
  // binary wrote; the two leaves now close over one file.
  //
  // Deliberately run as two DISPATCHES rather than by calling the engine
  // twice: the property is that the bytes one leaf writes are the bytes the
  // other reads, and only going through the file proves it.
  auto const fx        = make_fixture("wrbtrip");
  auto const state_dir = seed_org(fx);

  // Register a member so the round trip carries a project rather than being
  // trivially true over an empty table.
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    auto stmt = (*conn)->prepare("insert into projects (slug, name, root_path) values ('alpha', 'alpha', ?)");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_text(1, (fx.root / "proj").string()).has_value());
    REQUIRE(stmt->step().has_value());
    REQUIRE((*conn)
                ->execute("insert into project_associations (project_id, association_id, source) values (1, 1, 'user')")
                .has_value());
  }

  auto const built = dispatch(fx, {"workspace", "routing", "build"});
  REQUIRE(built.code == 0);
  CHECK(built.out.contains("(1 projects, 0 cross-repo deps)"));

  auto const shown = dispatch(fx, {"workspace", "routing", "show"});
  CHECK(shown.code == 0);
  CHECK(shown.err.empty());
  CHECK(shown.out.starts_with("workspace: org:acme (id 1)\n"));
  CHECK(shown.out.contains("projects:  1\n"));
  CHECK(shown.out.contains("- alpha\n"));

  // And the --json arm cats back the very bytes `build` wrote.
  auto const as_json = dispatch(fx, {"workspace", "routing", "show", "--json"});
  CHECK(as_json.code == 0);
  std::ifstream     input(state_dir / "routing-table.json", std::ios::binary);
  std::string const raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  CHECK_FALSE(raw.empty());
  CHECK(as_json.out == raw);
}

// =========================================================================
// task 6037 — the ten ported `workbench` leaves.
//
// ORACLE PROVENANCE. Captured under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/PLANAR_LOCAL_HOME/
// PLANAR_WORKBENCH_ROOT/HOME, `cd` FIRST then `env` (the reverse silently
// does not export past the `&&` on this platform's /bin/sh), each
// invocation redirected to its OWN capture file (the Zig runtime uses
// POSITIONAL writes, so two invocations sharing one target overwrite each
// other):
//
//   $Z workbench list                  -> exit 0, b'no features found\n'
//   $Z workbench list --json           -> exit 0, b'[]\n'
//   $Z workbench status                -> exit 0, b'no active features found\n'
//   $Z workbench status --json         -> exit 0, the SHORT totals payload
//   $Z workbench push 999              -> exit 1, b'error: plan not found: 999\n'
//   $Z workbench push nosuchslug       -> exit 1, b'error: plan not found: nosuchslug\n'
//   $Z workbench push 0                -> exit 2, b"error: invalid plan '0'\n"
//   $Z workbench push 1 --filter-mode nope
//     exit 2, b"error: invalid --filter-mode 'nope' (expected 'failures' or 'all')\n"
//   $Z workbench push 1 --filter-mode all --apply-cleanup
//     exit 2, b'error: --apply-cleanup is mutually exclusive with --filter-mode all\n'
//   $Z workbench gc                    -> exit 2,
//     b'error: plan argument required unless --all-scopes is set\n'
//   $Z workbench lint                  -> exit 2, and so does `lint 1 --all`,
//     b'error: choose exactly one lint target: <plan>, --all, or --path <file-or-directory>\n'
//   $Z workbench lint --path /nope.md  -> exit 1, b'error: lint target not found: /nope.md\n'
//   $Z workbench lint --path x.sh      -> exit 2,
//     b'error: lint target must be a Markdown file or directory: x.sh\n'
//   $Z workbench resolve abc --prefer fs
//     exit 2, b"error: event-id must be an integer, got 'abc'\n"
//   $Z workbench resolve 1 --prefer sideways
//     exit 2, b"error: --prefer must be fs|db, got 'sideways'\n"
//   $Z workbench resolve 99 --prefer fs
//     exit 1, b'error: workbench resolve failed: NotFound\n'
//   $Z workbench gc <plan>   (one drifted terminal file, no --yes)
//     exit 1, and NOTHING on either stream. That is an ORACLE DEFECT
//     reproduced deliberately -- see task 6122 and the comment in
//     handlers/workbench.cpp. Probed with stderr on a terminal, not
//     redirected, so it is not a capture artifact.
//
// Every one of these was ALSO run live through BOTH binaries in identical
// pinned arenas and diffed on stdout, stderr and exit code.

namespace {

/// @brief Give `fx` a workbench root inside its own scratch tree.
///
/// Without this the resolver would fall back to `$HOME/.planar/workbench`
/// -- which is still inside the fixture (its `HOME` is `<root>/fakehome`),
/// but naming the root explicitly is what makes the safety obvious at the
/// call site rather than two indirections away.
auto with_workbench_root(fixture& fx) -> std::filesystem::path {
  auto const root = fx.root / "wb";
  fx.vars.emplace("PLANAR_WORKBENCH_ROOT", root.string());
  return root;
}

/// @brief Seed one association-scoped anchor plan with one task, directly.
///
/// The planning verbs are not ported, so the rows go in through SQL --
/// which is legitimate here because the WORKBENCH surface is what is under
/// test, and the same shapes were verified against an oracle-seeded arena.
struct wb_seed {
  std::int64_t plan_id = 0;
  std::int64_t task_id = 0;
};

auto seed_workbench(const fixture& fx) -> wb_seed {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  auto const run = [&](std::string_view sql) { REQUIRE(conn->execute(sql).has_value()); };
  auto const id  = [&](std::string_view sql) {
    auto stmt = conn->prepare(sql);
    REQUIRE(stmt.has_value());
    auto step = stmt->step();
    REQUIRE(step.has_value());
    REQUIRE(*step == planar::db::step_result::row);
    return stmt->column_int64(0);
  };
  run("insert into associations (slug, name, kind) values ('project:demo', 'demo', 'project')");
  auto const assoc = id("select id from associations where slug = 'project:demo'");
  run(std::format("insert into plans (scope_kind, scope_id, title, slug, status) "
                  "values ('association', {}, 'Demo Feature', 'demo-feature', 'draft')",
                  assoc));
  wb_seed out;
  out.plan_id = id("select id from plans where slug = 'demo-feature'");
  run(std::format("insert into tasks (scope_kind, scope_id, plan_id, title, status, priority) "
                  "values ('association', {}, {}, 'First Task', 'todo', 100)",
                  assoc, out.plan_id));
  out.task_id = id("select id from tasks where title = 'First Task'");
  return out;
}

} // namespace

TEST_CASE("workbench list and status on an empty database", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wblist");
  with_workbench_root(fx);
  CHECK(dispatch(fx, {"workbench", "list"}).out == "no features found\n");
  CHECK(dispatch(fx, {"workbench", "list", "--json"}).out == "[]\n");
  CHECK(dispatch(fx, {"workbench", "status"}).out == "no active features found\n");
  auto const totals = dispatch(fx, {"workbench", "status", "--json"});
  CHECK(totals.code == 0);
  // The SHORT payload: no `filter_mode`, no `entries`.
  CHECK(totals.out == "{\"applied\":0,\"pending\":0,\"conflicts\":0,\"malformed\":0,\"malformed_files\":[],"
                      "\"filtered\":0,\"pre_existing_terminal\":0,\"cleaned\":0}\n");
}

TEST_CASE("workbench list CREATES the workbench root; status does not", "[cmd][handlers][workbench][parity]") {
  // The Zig split between `resolveAndEnsureWorkbenchRoot` and a plain
  // `resolveRoot`, reproduced. It decides whether a bare `workbench status`
  // on a fresh machine leaves a directory behind.
  auto       fx   = make_fixture("wbroot");
  auto const root = with_workbench_root(fx);
  CHECK(dispatch(fx, {"workbench", "status"}).code == 0);
  CHECK_FALSE(std::filesystem::exists(root));
  CHECK(dispatch(fx, {"workbench", "list"}).code == 0);
  CHECK(std::filesystem::exists(root));
}

TEST_CASE("workbench plan-argument failures split across exit 1 and exit 2", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wbplan");
  with_workbench_root(fx);
  seed_workbench(fx);

  auto const missing = dispatch(fx, {"workbench", "push", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: plan not found: 999\n");

  auto const slug = dispatch(fx, {"workbench", "push", "nosuchslug"});
  CHECK(slug.code == 1);
  CHECK(slug.err == "error: plan not found: nosuchslug\n");

  // Zero and negatives are INVALID, not not-found -- a different code and a
  // different sentence.
  auto const zero = dispatch(fx, {"workbench", "push", "0"});
  CHECK(zero.code == 2);
  CHECK(zero.err == "error: invalid plan '0'\n");
}

TEST_CASE("workbench push writes the tree and reports its summary line", "[cmd][handlers][workbench][parity]") {
  auto       fx   = make_fixture("wbpush");
  auto const root = with_workbench_root(fx);
  auto const seed = seed_workbench(fx);

  auto const pushed = dispatch(fx, {"workbench", "push", std::to_string(seed.plan_id)});
  CHECK(pushed.code == 0);
  CHECK(pushed.err.empty());
  CHECK(pushed.out == std::format("workbench push: plan {} (demo-feature) - 2 applied, 0 pending, 0 filtered "
                                  "(mode=failures), 0 conflict(s)\n",
                                  seed.plan_id));
  auto const dir = root / "project_demo" / std::format("p{}-demo-feature", seed.plan_id);
  CHECK(std::filesystem::exists(dir / "README.md"));
  CHECK(std::filesystem::exists(dir / ".sync"));

  // A second push is a clean no-op.
  auto const again = dispatch(fx, {"workbench", "push", std::to_string(seed.plan_id)});
  CHECK(again.out.find("0 applied") != std::string::npos);
}

TEST_CASE("workbench push refuses a bad --filter-mode and the excluded flag pair", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wbmode");
  with_workbench_root(fx);
  auto const seed = seed_workbench(fx);
  auto const plan = std::to_string(seed.plan_id);

  auto const bad = dispatch(fx, {"workbench", "push", plan, "--filter-mode", "nope"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: invalid --filter-mode 'nope' (expected 'failures' or 'all')\n");

  auto const excluded = dispatch(fx, {"workbench", "push", plan, "--filter-mode", "all", "--apply-cleanup"});
  CHECK(excluded.code == 2);
  CHECK(excluded.err == "error: --apply-cleanup is mutually exclusive with --filter-mode all\n");
  // Both refusals happen BEFORE any filesystem work.
  CHECK(excluded.out.empty());
}

TEST_CASE("a malformed workbench file makes pull exit 1", "[cmd][handlers][workbench][parity]") {
  auto       fx   = make_fixture("wbmal");
  auto const root = with_workbench_root(fx);
  auto const seed = seed_workbench(fx);
  auto const plan = std::to_string(seed.plan_id);
  REQUIRE(dispatch(fx, {"workbench", "push", plan}).code == 0);

  auto const dir = root / "project_demo" / std::format("p{}-demo-feature", seed.plan_id);
  REQUIRE(planar::engine::workbench::fsutil::write_file_atomic(dir / "tasks" / "cross" / "junk.md", "broken\n"));

  auto const pulled = dispatch(fx, {"workbench", "pull", plan, "--verbose"});
  CHECK(pulled.code == 1);
  CHECK(pulled.out.find("MALFORMED: project_demo/") != std::string::npos);
  CHECK(pulled.out.find("(MalformedFrontmatter)") != std::string::npos);
  CHECK(pulled.err ==
        std::format("error: {} malformed workbench file(s); run 'planar workbench lint {}' for details\n", 1, plan));
}

TEST_CASE("a conflict makes pull exit 3, and status exit 0", "[cmd][handlers][workbench][parity]") {
  // The exit-code split matters: `status` REPORTS conflicts without failing,
  // while every writing verb fails on them.
  auto       fx   = make_fixture("wbconf");
  auto const root = with_workbench_root(fx);
  auto const seed = seed_workbench(fx);
  auto const plan = std::to_string(seed.plan_id);
  REQUIRE(dispatch(fx, {"workbench", "push", plan}).code == 0);

  auto const dir  = root / "project_demo" / std::format("p{}-demo-feature", seed.plan_id);
  auto const file = dir / "tasks" / "cross" / std::format("{}-first-task.md", seed.task_id);
  auto const body = planar::engine::workbench::fsutil::read_file(file);
  REQUIRE(body.has_value());
  REQUIRE(planar::engine::workbench::fsutil::write_file_atomic(file, *body + "FS side extra\n"));
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute(std::format("update tasks set body = 'DB side', updated_at = "
                                      "strftime('%Y-%m-%dT%H:%M:%fZ','now','+1 second') where id = {}",
                                      seed.task_id))
                .has_value());
  }

  auto const peek = dispatch(fx, {"workbench", "status", plan});
  CHECK(peek.code == 0);
  CHECK(peek.out.find("CONFLICT [") != std::string::npos);

  auto const pulled = dispatch(fx, {"workbench", "pull", plan});
  CHECK(pulled.code == 3);
  CHECK(pulled.err == "error: 1 conflict(s) require 'workbench resolve <event-id> --prefer fs|db'\n");
}

TEST_CASE("workbench resolve validates its argument and its flag before the engine", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wbres");
  with_workbench_root(fx);
  seed_workbench(fx);

  auto const not_int = dispatch(fx, {"workbench", "resolve", "abc", "--prefer", "fs"});
  CHECK(not_int.code == 2);
  CHECK(not_int.err == "error: event-id must be an integer, got 'abc'\n");

  auto const bad_prefer = dispatch(fx, {"workbench", "resolve", "1", "--prefer", "sideways"});
  CHECK(bad_prefer.code == 2);
  CHECK(bad_prefer.err == "error: --prefer must be fs|db, got 'sideways'\n");

  // An unknown event is exit 1; the message interpolates the raw Zig tag.
  auto const missing = dispatch(fx, {"workbench", "resolve", "99", "--prefer", "fs"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: workbench resolve failed: NotFound\n");
}

TEST_CASE("workbench gc requires a plan unless --all-scopes", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wbgc");
  with_workbench_root(fx);
  auto const seed = seed_workbench(fx);

  auto const bare = dispatch(fx, {"workbench", "gc"});
  CHECK(bare.code == 2);
  CHECK(bare.err == "error: plan argument required unless --all-scopes is set\n");

  auto const swept = dispatch(fx, {"workbench", "gc", "--all-scopes", "--json"});
  CHECK(swept.code == 0);
  CHECK(swept.out ==
        "{\"removed\":0,\"kept\":0,\"drifted_skipped\":0,\"errors\":0,\"dry_run\":false,\"filter_mode\":\"failures\"}\n");
  static_cast<void>(seed);
}

TEST_CASE("the gc drift refusal names the held-back files instead of exiting silently",
          "[cmd][handlers][workbench][sanctioned-divergence]") {
  // DIVERGES FROM THE ORACLE ON PURPOSE (task 6122). The Zig handler writes
  // its refusal to the buffered ctx.stderr and then calls std.process.exit(1)
  // directly, skipping the runtime shutdown that would flush it, so the
  // operator gets exit 1 and no reason at all. Verified against the oracle
  // with stderr on a terminal.
  //
  // This test previously pinned the reproduced SILENCE. Emitting was believed
  // to cost a differential-harness regression; it does not, because neither
  // harness drifts a file (see the comment on the handler). The message is
  // `render_cli::render_gc_drift_refusal`, pinned bytewise in
  // render_cli.t.cpp; this case pins that the handler actually emits it, on
  // stderr, with the exit code unchanged.
  auto       fx   = make_fixture("wbdrift");
  auto const root = with_workbench_root(fx);
  auto const seed = seed_workbench(fx);
  auto const plan = std::to_string(seed.plan_id);
  REQUIRE(dispatch(fx, {"workbench", "push", plan}).code == 0);

  auto const dir  = root / "project_demo" / std::format("p{}-demo-feature", seed.plan_id);
  auto const file = dir / "tasks" / "cross" / std::format("{}-first-task.md", seed.task_id);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute(std::format("update tasks set status = 'cancelled' where id = {}", seed.task_id)).has_value());
  }
  auto const body = planar::engine::workbench::fsutil::read_file(file);
  REQUIRE(body.has_value());
  REQUIRE(planar::engine::workbench::fsutil::write_file_atomic(file, *body + "unpulled edit\n"));

  auto const refused = dispatch(fx, {"workbench", "gc", plan});
  CHECK(refused.code == 1);
  CHECK(refused.out.empty()); // stderr, not stdout
  CHECK(refused.err.starts_with("workbench gc: refused to remove 1 file(s) with FS-content drift from DB; re-run with --yes "
                                "to discard, or 'workbench pull' first\n"));
  // The refusal NAMES the file it held back -- a count alone would not tell
  // an operator which path to `workbench pull`.
  CHECK(refused.err.contains(std::format("  drift: {}\n", file.string())));
  CHECK(std::filesystem::exists(file)); // held back, not removed

  // `--yes` proceeds, and prints normally.
  auto const forced = dispatch(fx, {"workbench", "gc", plan, "--yes"});
  CHECK(forced.code == 0);
  CHECK(forced.out.find("removed 1") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("workbench lint demands EXACTLY one target", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wblint");
  with_workbench_root(fx);
  auto const seed = seed_workbench(fx);

  constexpr std::string_view expected = "error: choose exactly one lint target: <plan>, --all, or --path <file-or-directory>\n";
  auto const                 none     = dispatch(fx, {"workbench", "lint"});
  CHECK(none.code == 2);
  CHECK(none.err == expected);
  auto const two = dispatch(fx, {"workbench", "lint", std::to_string(seed.plan_id), "--all"});
  CHECK(two.code == 2);
  CHECK(two.err == expected);
}

TEST_CASE("workbench lint distinguishes a missing target from a non-Markdown one", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wblint2");
  with_workbench_root(fx);
  seed_workbench(fx);

  auto const absent = dispatch(fx, {"workbench", "lint", "--path", (fx.root / "nope.md").string()});
  CHECK(absent.code == 1);
  CHECK(absent.err == std::format("error: lint target not found: {}\n", (fx.root / "nope.md").string()));

  auto const script = fx.root / "run.sh";
  REQUIRE(planar::engine::workbench::fsutil::write_file_atomic(script, "#!/bin/sh\n"));
  auto const wrong_kind = dispatch(fx, {"workbench", "lint", "--path", script.string()});
  CHECK(wrong_kind.code == 2);
  CHECK(wrong_kind.err == std::format("error: lint target must be a Markdown file or directory: {}\n", script.string()));
}

TEST_CASE("workbench lint reports a coded diagnostic and fails on WARNINGS alone", "[cmd][handlers][workbench][parity]") {
  auto fx = make_fixture("wblint3");
  with_workbench_root(fx);
  seed_workbench(fx);

  auto const bad = fx.root / "bad_status.md";
  REQUIRE(planar::engine::workbench::fsutil::write_file_atomic(
      bad, "---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: bogus\n---\n"));
  auto const errored = dispatch(fx, {"workbench", "lint", "--path", bad.string()});
  CHECK(errored.code == 1);
  CHECK(errored.out == std::format("{}:5:\n"
                                   "  error[invalid_field_value]: front matter field 'status' has an unsupported "
                                   "value\n"
                                   "  hint: use one of: todo, doing, blocked, done, or cancelled\n"
                                   "1 files scanned, 1 errors, 0 warnings.\n",
                                   bad.string()));
  CHECK(errored.err == "error: workbench lint found 1 error(s) and 0 warning(s)\n");

  // Warnings alone still fail the verb.
  auto const warned_file = fx.root / "no_anchor.md";
  REQUIRE(planar::engine::workbench::fsutil::write_file_atomic(
      warned_file, "---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\n---\n"));
  auto const warned = dispatch(fx, {"workbench", "lint", "--path", warned_file.string(), "--json"});
  CHECK(warned.code == 1);
  CHECK(warned.out.find("\"severity\":\"warning\",\"code\":\"anchor_plan_not_found\"") != std::string::npos);
  CHECK(warned.err == "error: workbench lint found 0 error(s) and 1 warning(s)\n");
}

TEST_CASE("workbench archive then restore round-trips the tree", "[cmd][handlers][workbench][parity]") {
  auto       fx   = make_fixture("wbarch");
  auto const root = with_workbench_root(fx);
  auto const seed = seed_workbench(fx);
  auto const plan = std::to_string(seed.plan_id);
  REQUIRE(dispatch(fx, {"workbench", "push", plan}).code == 0);

  auto const dir      = root / "project_demo" / std::format("p{}-demo-feature", seed.plan_id);
  auto const archived = dispatch(fx, {"workbench", "archive", plan, "--json"});
  CHECK(archived.code == 0);
  CHECK(archived.out == std::format("{{\"archived\":true,\"plan\":{},\"feature_dir\":\"{}\"}}\n", plan, dir.string()));
  CHECK_FALSE(std::filesystem::exists(dir));

  auto const restored = dispatch(fx, {"workbench", "restore", plan});
  CHECK(restored.code == 0);
  CHECK(restored.out == std::format("restored: {} (plan {})\n", dir.string(), plan));
  CHECK(std::filesystem::exists(dir / "README.md"));
}

// =========================================================================
// Agent-lifecycle operator verbs (plan 996, task 6040)
// =========================================================================
//
// ORACLE PROVENANCE. Captured by running the Zig binary in a pinned scratch
// arena (`cd` FIRST, then `env` — `VAR=x cd dir && bin` does NOT export
// past the `&&` on this platform's /bin/sh) and, for the seeded cases, by
// seeding with the ORACLE because this tree has no planning verbs to seed
// with:
//
//   $Z capture session --json      -> {"ok":true,"id":1,"vendor":"cli"}
//   $Z capture session             -> session 1 opened (vendor: cli)
//   $Z capture note "hello" --json -> {"ok":true,"session_id":1}
//   $Z capture note "second"       -> captured note in session 1
//   $Z capture command "ls -la" --outcome ok --json -> {"ok":true,"session_id":1}
//   $Z capture file /tmp/x.txt --role input --json  -> {"ok":true,"session_id":1}
//   $Z capture snapshot --note "b" --json
//       -> {"ok":true,"id":1,"session_id":1,"vendor":"cli"}
//   $Z capture end --json          -> {"ok":true,"id":1}
//   $Z capture end 1 --json        -> exit 1, error: session 1 is already ended
//   $Z capture end abc --json      -> exit 2,
//                                     error: session id must be an integer, got 'abc'
//   $Z capture end 999 --json      -> exit 1, error: session 999 not found
//   $Z capture end                 -> exit 1, error: no active session
//   $Z handoff                     -> exit 2, error: no active session
//                                     (run `planar capture session` first)
//   $Z handoff list --json         -> ZERO BYTES
//   $Z handoff list                -> no handoffs
//   $Z handoff show 1 --json       -> exit 1, error: handoff 1 not found
//   $Z handoff show abc            -> exit 2,
//                                     error: handoff id must be an integer, got 'abc'
//   $Z handoff list --status bogus -> exit 2, error: unknown handoff status 'bogus'
//   $Z handoff create 99           -> exit 1, error: snapshot 99 not found
//   $Z resume validate 999 --json  -> exit 1, ZERO stdout,
//                                     error: task 999 not found
//   $Z resume validate abc         -> exit 2,
//                                     error: task id must be an integer, got 'abc'
//
// The C++ binary was diffed against the oracle over all of the above plus
// the full handoff lifecycle in two identically-seeded arenas; every case
// matched byte-for-byte once wall-clock timestamps were normalized.

/// @brief Seed a task with an explicit `next_action` directly, since no
/// planning verb is ported into this binary.
namespace {

auto seed_task(const fixture& fx, std::string_view title, std::optional<std::string_view> next_action) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  if (next_action.has_value()) {
    REQUIRE(conn->execute(std::format("insert into tasks (scope_kind, title, status, priority, next_action) "
                                      "values ('global', '{}', 'todo', 100, '{}')",
                                      title, *next_action))
                .has_value());
  } else {
    REQUIRE(conn->execute(std::format("insert into tasks (scope_kind, title, status, priority) "
                                      "values ('global', '{}', 'todo', 100)",
                                      title))
                .has_value());
  }
  auto stmt = conn->prepare("select max(id) from tasks");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  return stmt->column_int64(0);
}

/// @brief The `sessions` row columns task 6128 is about, with NULL kept
/// DISTINGUISHABLE from the empty string.
///
/// That distinction is the whole point. `capture session` exited 0 with the
/// oracle's byte-identical stdout while writing SQL NULL into both columns;
/// no exit code and no stdout diff can see it, and a reader that collapsed
/// NULL to `""` could not either.
struct session_git_row {
  std::int64_t id                = 0;    ///< The session id.
  bool         repo_root_is_null = true; ///< `repo_root IS NULL`.
  bool         head_sha_is_null  = true; ///< `head_sha_at_start IS NULL`.
  std::string  repo_root;                ///< The value, when not NULL.
  std::string  head_sha_at_start;        ///< The value, when not NULL.
};

/// @brief Read the newest `sessions` row's git columns.
/// @param fx The fixture.
/// @return The row, or unset when there is none.
auto read_session_git(const fixture& fx) -> std::optional<session_git_row> {
  auto conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return std::nullopt;
  }
  auto stmt = conn->prepare("select id, repo_root, head_sha_at_start from sessions order by id desc limit 1");
  if (!stmt) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return std::nullopt;
  }
  return session_git_row{
      .id                = stmt->column_int64(0),
      .repo_root_is_null = stmt->is_null(1),
      .head_sha_is_null  = stmt->is_null(2),
      .repo_root         = stmt->is_null(1) ? std::string{} : stmt->column_text(1),
      .head_sha_at_start = stmt->is_null(2) ? std::string{} : stmt->column_text(2),
  };
}

/// @brief Turn the fixture's `proj` directory into a repository with one
/// commit.
/// @param fx The fixture.
/// @return True when every fixture step succeeded.
auto seed_git_commit(const fixture& fx) -> bool {
  auto const dir  = (fx.root / "proj").string();
  auto const line = std::format("git -C '{0}' init -q -b main && git -C '{0}' config user.email planar@example.invalid && "
                                "git -C '{0}' config user.name Planar && git -C '{0}' commit -q --allow-empty -m seed",
                                dir);
  return std::system(std::format("{} >/dev/null 2>&1", line).c_str()) == 0;
}

} // namespace

TEST_CASE("capture session opens a session and reuses it on a second call", "[cmd][handlers][capture]") {
  auto const fx = make_fixture("capsession");

  auto const first = dispatch(fx, {"capture", "session", "--json"});
  CHECK(first.code == 0);
  CHECK(first.out == "{\"ok\":true,\"id\":1,\"vendor\":\"cli\"}\n");
  CHECK(first.err.empty());
  CHECK(first.db_open);

  // Same vendor tuple -> the SAME row id. `capture session` is open-or-reuse.
  auto const second = dispatch(fx, {"capture", "session"});
  CHECK(second.code == 0);
  CHECK(second.out == "session 1 opened (vendor: cli)\n");
}

TEST_CASE("capture session STAMPS repo_root and head_sha_at_start onto the row", "[cmd][handlers][capture][6128]") {
  // THE 6128 GUARD, and the reason it asserts on COLUMNS.
  //
  // Before this change the handler called `open_session` with three fields
  // and let the optional git context default to `nullopt`, so every session
  // this binary ever created carried `repo_root = NULL` and
  // `head_sha_at_start = NULL` — while exiting 0 with stdout byte-identical
  // to the oracle's. The engine's own unit tests passed throughout, because
  // they exercise the stamping arm the handler never took.
  //
  // `capture commits` is documented as a no-op whenever either column is
  // NULL, so those sessions looked captured and reconciled to nothing. The
  // ONLY observable that can tell the fixed handler from the broken one is
  // the row, with NULL held distinct from "".
  auto const fx = make_fixture("capgit");
  if (!seed_git_commit(fx)) {
    SKIP("git unavailable — the stamping arm cannot be exercised");
  }

  auto const got = dispatch(fx, {"capture", "session", "--json"});
  CHECK(got.code == 0);
  // Unchanged stdout is part of the contract, not incidental: the fix must
  // not move the bytes the oracle emits.
  CHECK(got.out == "{\"ok\":true,\"id\":1,\"vendor\":\"cli\"}\n");

  auto const row = read_session_git(fx);
  REQUIRE(row.has_value());
  CHECK(row->id == 1);
  // NOT NULL, and not the empty string either — a stamping path that wrote
  // `""` would satisfy a naive `IS NOT NULL` check and still be wrong.
  CHECK_FALSE(row->repo_root_is_null);
  CHECK_FALSE(row->head_sha_is_null);
  CHECK_FALSE(row->repo_root.empty());
  CHECK(row->head_sha_at_start.size() == 40);
  CHECK(row->head_sha_at_start.find_first_not_of("0123456789abcdef") == std::string::npos);
  // The root recorded is THIS repository. Compared canonically because the
  // fixture lives under a temp dir that is itself a symlink on macOS.
  CHECK(std::filesystem::canonical(row->repo_root) == std::filesystem::canonical(fx.root / "proj"));
}

TEST_CASE("capture session leaves both git columns NULL outside a repository", "[cmd][handlers][capture][6128]") {
  // The other half of the contract, and the reason the fix is a PROBE and
  // not an unconditional write. The oracle skips the stamping step whenever
  // its own probe fails, so outside a repository the correct row is exactly
  // the row the BROKEN handler wrote. Asserting only the positive arm would
  // let an implementation that fabricates a value pass.
  auto const fx  = make_fixture("capnogit");
  auto const got = dispatch(fx, {"capture", "session", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out == "{\"ok\":true,\"id\":1,\"vendor\":\"cli\"}\n");

  auto const row = read_session_git(fx);
  REQUIRE(row.has_value());
  CHECK(row->repo_root_is_null);
  CHECK(row->head_sha_is_null);
}

TEST_CASE("capture session leaves both git columns NULL before the first commit", "[cmd][handlers][capture][6128]") {
  // A repository with no commits ANSWERS `rev-parse --show-toplevel` and
  // REFUSES `rev-parse HEAD`. The two columns are written together or not
  // at all: a half-stamped row (a root with no sha) is the degraded shape
  // this task is about, arrived at from the other direction.
  auto const fx  = make_fixture("capnocommit");
  auto const dir = (fx.root / "proj").string();
  if (std::system(std::format("git -C '{}' init -q -b main >/dev/null 2>&1", dir).c_str()) != 0) {
    SKIP("git unavailable");
  }

  auto const got = dispatch(fx, {"capture", "session", "--json"});
  CHECK(got.code == 0);

  auto const row = read_session_git(fx);
  REQUIRE(row.has_value());
  CHECK(row->repo_root_is_null);
  CHECK(row->head_sha_is_null);
}

TEST_CASE("capture session's stamp is write-once across a reuse", "[cmd][handlers][capture][6128]") {
  // `capture session` is open-or-reuse, and the engine stamps through
  // `set_start_git_context_if_unset`. The second call must not re-stamp:
  // the whole point of `head_sha_at_start` is that it is the HEAD the
  // session STARTED at, so a session that keeps re-reading HEAD as commits
  // land would make the commit window collapse to nothing.
  auto const fx = make_fixture("capgitreuse");
  if (!seed_git_commit(fx)) {
    SKIP("git unavailable");
  }

  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);
  auto const first = read_session_git(fx);
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first->head_sha_is_null);

  // Move HEAD, then reuse the same vendor tuple.
  auto const dir = (fx.root / "proj").string();
  REQUIRE(std::system(std::format("git -C '{}' commit -q --allow-empty -m second >/dev/null 2>&1", dir).c_str()) == 0);
  auto const second_call = dispatch(fx, {"capture", "session"});
  CHECK(second_call.code == 0);
  CHECK(second_call.out == "session 1 opened (vendor: cli)\n");

  auto const second = read_session_git(fx);
  REQUIRE(second.has_value());
  CHECK(second->id == first->id);
  CHECK(second->head_sha_at_start == first->head_sha_at_start);
}

TEST_CASE("capture session honours --vendor over the environment", "[cmd][handlers][capture]") {
  auto fx                  = make_fixture("capvendor");
  fx.vars["PLANAR_VENDOR"] = "claude";

  // The environment supplies the vendor when no flag does...
  auto const from_env = dispatch(fx, {"capture", "session", "--json"});
  CHECK(from_env.code == 0);
  CHECK(from_env.out == "{\"ok\":true,\"id\":1,\"vendor\":\"claude\"}\n");

  // ...and the flag wins over it, opening a DIFFERENT session because the
  // vendor tuple differs.
  auto const from_flag = dispatch(fx, {"capture", "session", "--vendor", "codex", "--json"});
  CHECK(from_flag.code == 0);
  CHECK(from_flag.out == "{\"ok\":true,\"id\":2,\"vendor\":\"codex\"}\n");
}

TEST_CASE("an EMPTY PLANAR_VENDOR reads as absent, not as an empty vendor", "[cmd][handlers][capture]") {
  auto fx                             = make_fixture("capemptyvendor");
  fx.vars["PLANAR_VENDOR"]            = "";
  fx.vars["PLANAR_VENDOR_SESSION_ID"] = "";

  // The Zig original's `if (v.len > 0)` guard. Without it the vendor would
  // be "" and the JSON would read `"vendor":""`.
  auto const got = dispatch(fx, {"capture", "session", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out == "{\"ok\":true,\"id\":1,\"vendor\":\"cli\"}\n");
  CHECK_FALSE(got.out.contains("vendor_session_id"));
}

TEST_CASE("capture session surfaces the vendor session id and task binding", "[cmd][handlers][capture]") {
  auto const fx   = make_fixture("capvsid");
  auto const task = seed_task(fx, "Bound task", "do it");

  auto const got = dispatch(
      fx, {"capture", "session", "--vendor-session-id", "abc", "--model", "m1", "--task", std::to_string(task), "--json"});
  CHECK(got.code == 0);
  // `--model` is accepted and stored but does NOT appear in the envelope.
  CHECK(got.out ==
        std::format("{{\"ok\":true,\"id\":1,\"vendor\":\"cli\",\"vendor_session_id\":\"abc\",\"task_id\":{}}}\n", task));
}

TEST_CASE("capture note/command/file create a session when none exists", "[cmd][handlers][capture]") {
  auto const fx = make_fixture("capappend");

  // No prior `capture session`: the append leaves resolve through
  // `ensure_active`, which CREATES. This is why `planar capture note "x"`
  // works on a fresh database.
  auto const noted = dispatch(fx, {"capture", "note", "hello", "--json"});
  CHECK(noted.code == 0);
  CHECK(noted.out == "{\"ok\":true,\"session_id\":1}\n");

  auto const text = dispatch(fx, {"capture", "note", "second"});
  CHECK(text.out == "captured note in session 1\n");

  auto const commanded = dispatch(fx, {"capture", "command", "ls -la", "--outcome", "ok", "--json"});
  CHECK(commanded.code == 0);
  CHECK(commanded.out == "{\"ok\":true,\"session_id\":1}\n");

  auto const filed = dispatch(fx, {"capture", "file", "/tmp/x.txt", "--role", "input", "--json"});
  CHECK(filed.code == 0);

  // The composed bodies are the observable part: `--outcome` puts the
  // outcome on a SECOND LINE, `--role` puts the role in brackets.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select prefix, body from session_entries order by ordinal");
  REQUIRE(stmt.has_value());
  std::vector<std::pair<std::string, std::string>> entries;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped != planar::db::step_result::row) {
      break;
    }
    entries.emplace_back(stmt->column_text(0), stmt->column_text(1));
  }
  REQUIRE(entries.size() == 4);
  CHECK(entries[0] == std::pair<std::string, std::string>{"note", "hello"});
  CHECK(entries[1] == std::pair<std::string, std::string>{"note", "second"});
  CHECK(entries[2] == std::pair<std::string, std::string>{"command", "ls -la\noutcome: ok"});
  CHECK(entries[3] == std::pair<std::string, std::string>{"file", "/tmp/x.txt [input]"});
}

TEST_CASE("capture end refuses when there is no active session", "[cmd][handlers][capture]") {
  auto const fx = make_fixture("capendnone");
  // `end` resolves through `active_for_vendor`, which does NOT create.
  // Collapsing it onto the append leaves' `ensure_active` would make this
  // silently open a session and end it.
  auto const got = dispatch(fx, {"capture", "end"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: no active session\n");
}

TEST_CASE("capture end: the three id paths and their refusals", "[cmd][handlers][capture]") {
  auto const fx = make_fixture("capend");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto const bad = dispatch(fx, {"capture", "end", "abc", "--json"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: session id must be an integer, got 'abc'\n");

  auto const absent = dispatch(fx, {"capture", "end", "999", "--json"});
  CHECK(absent.code == 1);
  CHECK(absent.err == "error: session 999 not found\n");

  auto const ended = dispatch(fx, {"capture", "end", "--summary", "wrapped", "--json"});
  CHECK(ended.code == 0);
  CHECK(ended.out == "{\"ok\":true,\"id\":1}\n");

  auto const again = dispatch(fx, {"capture", "end", "1", "--json"});
  CHECK(again.code == 1);
  CHECK(again.err == "error: session 1 is already ended\n");
}

TEST_CASE("capture snapshot inherits next_action from the bound task", "[cmd][handlers][capture]") {
  auto const fx   = make_fixture("capsnap");
  auto const task = seed_task(fx, "Has action", "continue the port");

  auto const got = dispatch(fx, {"capture", "snapshot", "--task", std::to_string(task), "--note", "body", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out == std::format("{{\"ok\":true,\"id\":1,\"session_id\":1,\"vendor\":\"cli\",\"task_id\":{}}}\n", task));

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select body, next_action from context_snapshots where id = 1");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_text(0) == "body");
  // No `--next-action` was passed; the task's own column supplied it.
  CHECK(stmt->column_text(1) == "continue the port");
}

TEST_CASE("capture snapshot: --note beats the positional body", "[cmd][handlers][capture]") {
  auto const fx = make_fixture("capsnapnote");

  auto const got = dispatch(fx, {"capture", "snapshot", "positional", "--note", "flagwins", "--json"});
  CHECK(got.code == 0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select body from context_snapshots where id = 1");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_text(0) == "flagwins");
}

// ---------------------------------------------------------------------------
// capture commits (plan 996, task 6358)
// ---------------------------------------------------------------------------

TEST_CASE("capture commits refuses --since combined with an explicit SHA", "[cmd][handlers][capture][commits]") {
  auto const fx = make_fixture("capcommitsmutex");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto const got = dispatch(fx, {"capture", "commits", "--since", "HEAD", "deadbeef"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: cannot combine --since with explicit commit SHAs\n");
}

TEST_CASE("capture commits refuses when neither --since nor a SHA is given", "[cmd][handlers][capture][commits]") {
  auto const fx = make_fixture("capcommitsneither");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto const got = dispatch(fx, {"capture", "commits"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: provide --since <ref> or one or more commit SHAs\n");
}

TEST_CASE("capture commits refuses with no active session", "[cmd][handlers][capture][commits]") {
  // `commits` shares `end`'s NON-creating session resolution, not the
  // append leaves' create-on-demand one. See handlers/capture.cppm's
  // header.
  auto const fx  = make_fixture("capcommitsnosess");
  auto const got = dispatch(fx, {"capture", "commits", "--since", "HEAD"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: no active session (run `planar capture session` first)\n");
}

TEST_CASE("capture commits reports session-not-found for an explicit --session", "[cmd][handlers][capture][commits]") {
  auto const fx = make_fixture("capcommitsnf");
  if (!seed_git_commit(fx)) {
    SKIP("git unavailable");
  }

  auto const got = dispatch(fx, {"capture", "commits", "--session", "9999", "--since", "HEAD"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: session 9999 not found\n");
}

TEST_CASE("capture commits reports NOT a git repository for a non-repo --repo", "[cmd][handlers][capture][commits]") {
  auto const fx = make_fixture("capcommitsnongit");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "nongit", ec);

  auto const got = dispatch(fx, {"capture", "commits", "--repo", (fx.root / "nongit").string(), "--since", "HEAD"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == std::format("error: repo is not a git repository: {}\n", (fx.root / "nongit").string()));
}

TEST_CASE("capture commits reports an unresolvable --since ref", "[cmd][handlers][capture][commits]") {
  auto const fx = make_fixture("capcommitsbadref");
  if (!seed_git_commit(fx)) {
    SKIP("git unavailable");
  }
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto const got = dispatch(fx, {"capture", "commits", "--since", "not-a-ref"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: cannot resolve ref 'not-a-ref'\n");
}

TEST_CASE("capture commits reports unresolvable SHAs distinctly from an unresolvable --since",
          "[cmd][handlers][capture][commits]") {
  auto const fx = make_fixture("capcommitsbadsha");
  if (!seed_git_commit(fx)) {
    SKIP("git unavailable");
  }
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto const got = dispatch(fx, {"capture", "commits", "not-a-real-sha"});
  CHECK(got.code == 1);
  CHECK(got.out.empty());
  CHECK(got.err == "error: one or more commit SHAs could not be resolved\n");
}

TEST_CASE("capture commits walks --since into new rows and is idempotent on re-run", "[cmd][handlers][capture][commits]") {
  // `--repo` is passed EXPLICITLY here rather than relying on its `.`
  // default: `dispatch`'s `context` carries a virtual `cwd()` for
  // DB/scope resolution, but the git subprocess this leaf shells reads
  // the REAL OS process cwd for a relative `-C .`, which this shared test
  // binary cannot safely repoint per test case. The `.` default itself is
  // a one-line `flag_string(args, "--repo").value_or(".")`, oracle-pinned
  // in capture.t.cpp's header transcript; what this case defends is the
  // WALK, not the default.
  auto const fx = make_fixture("capcommitswalk");
  if (!seed_git_commit(fx)) {
    SKIP("git unavailable");
  }
  REQUIRE(dispatch(fx, {"capture", "session", "--json"}).code == 0);

  auto const  dir  = (fx.root / "proj").string();
  auto const  base = std::format("git -C '{0}' rev-parse HEAD", dir);
  std::string base_sha;
  {
    FILE* pipe = popen(base.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
      base_sha += buf;
    }
    pclose(pipe);
    while (!base_sha.empty() && (base_sha.back() == '\n' || base_sha.back() == '\r')) {
      base_sha.pop_back();
    }
  }
  REQUIRE_FALSE(base_sha.empty());

  auto const zero = dispatch(fx, {"capture", "commits", "--repo", dir, "--since", base_sha, "--json"});
  CHECK(zero.code == 0);
  CHECK(zero.out.find("\"commit_count\":0") != std::string::npos);
  CHECK(zero.out.find("\"inserted_count\":0") != std::string::npos);

  REQUIRE(std::system(std::format("git -C '{}' commit -q --allow-empty -m second >/dev/null 2>&1", dir).c_str()) == 0);

  auto const one = dispatch(fx, {"capture", "commits", "--repo", dir, "--since", base_sha, "--json"});
  CHECK(one.code == 0);
  CHECK(one.out.find("\"commit_count\":1") != std::string::npos);
  CHECK(one.out.find("\"inserted_count\":1") != std::string::npos);

  auto const text = dispatch(fx, {"capture", "commits", "--repo", dir, "--since", base_sha});
  CHECK(text.code == 0);
  // Re-run: idempotent, so `commit_count` stays 1 but `inserted_count`
  // drops to zero.
  CHECK(text.out == "session 1: processed 1 commits (0 new)\n");
}

TEST_CASE("resume validate: absent task writes NOTHING to stdout", "[cmd][handlers][resume]") {
  auto const fx  = make_fixture("rvabsent");
  auto const got = dispatch(fx, {"resume", "validate", "999", "--json"});
  CHECK(got.code == 1);
  // The distinguishing property: "absent" emits no payload at all, where
  // "present but not resumable" emits a full one. A caller can tell them
  // apart without parsing stderr.
  CHECK(got.out.empty());
  CHECK(got.err == "error: task 999 not found\n");
}

TEST_CASE("resume validate: a bad id is exit 2 from the HANDLER", "[cmd][handlers][resume]") {
  auto const fx  = make_fixture("rvbad");
  auto const got = dispatch(fx, {"resume", "validate", "abc", "--json"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  // The positional is declared as a STRING in the tree precisely so this
  // wording survives; an int validator would answer CLI11's instead.
  CHECK(got.err == "error: task id must be an integer, got 'abc'\n");
}

TEST_CASE("resume validate: a NON-resumable task still writes its payload, then exits 1", "[cmd][handlers][resume]") {
  auto const fx   = make_fixture("rvfail");
  auto const task = seed_task(fx, "No action", std::nullopt);

  auto const got = dispatch(fx, {"resume", "validate", std::to_string(task), "--json"});
  CHECK(got.code == 1);
  CHECK(got.err == std::format("error: task {} is not resumable\n", task));
  // THE POINT: stdout carries the diagnosis even though the exit is
  // non-zero. A caller reading stdout only on exit 0 loses exactly the
  // failure list it needs.
  CHECK(got.out == std::format("{{\"task_id\":{},\"resumable\":false,\"failures\":["
                               "{{\"check\":\"next_action\",\"message\":\"next_action is null\","
                               "\"remediation\":\"planar task update {} --next-action \\\"<text>\\\"\"}},"
                               "{{\"check\":\"snapshot\",\"message\":\"no context snapshot found\","
                               "\"remediation\":\"planar capture snapshot --task {}\"}}]}}\n",
                               task, task, task));
}

TEST_CASE("resume validate: text form lists each failure with its remediation", "[cmd][handlers][resume]") {
  auto const fx   = make_fixture("rvfailtext");
  auto const task = seed_task(fx, "No action", std::nullopt);

  auto const got = dispatch(fx, {"resume", "validate", std::to_string(task)});
  CHECK(got.code == 1);
  CHECK(got.out == std::format("FAIL task:{} is not resumable:\n"
                               "  - next_action is null \xe2\x86\x92 run: planar task update {} "
                               "--next-action \"<text>\"\n"
                               "  - no context snapshot found \xe2\x86\x92 run: planar capture snapshot --task {}\n",
                               task, task, task));
}

TEST_CASE("resume validate: the full gate goes green once BOTH rules are met", "[cmd][handlers][resume]") {
  auto const fx   = make_fixture("rvpass");
  auto const task = seed_task(fx, "Has action", "do the thing");

  // next_action alone is not enough.
  auto const halfway = dispatch(fx, {"resume", "validate", std::to_string(task), "--json"});
  CHECK(halfway.code == 1);
  CHECK(halfway.out.contains("\"check\":\"snapshot\""));
  CHECK_FALSE(halfway.out.contains("\"check\":\"next_action\""));

  // A SESSION-level snapshot still is not enough — it has no task binding.
  REQUIRE(dispatch(fx, {"capture", "snapshot", "--note", "sessionwide"}).code == 0);
  auto const still = dispatch(fx, {"resume", "validate", std::to_string(task), "--json"});
  CHECK(still.code == 1);
  CHECK(still.out.contains("\"check\":\"snapshot\""));

  // A TASK-SCOPED snapshot flips it.
  REQUIRE(dispatch(fx, {"capture", "snapshot", "--task", std::to_string(task), "--note", "checkpoint"}).code == 0);
  auto const green = dispatch(fx, {"resume", "validate", std::to_string(task), "--json"});
  CHECK(green.code == 0);
  CHECK(green.err.empty());
  // `null`, NOT `[]`. The handoff composite emits `[]` for this same state.
  CHECK(green.out == std::format("{{\"task_id\":{},\"resumable\":true,\"failures\":null}}\n", task));

  auto const green_text = dispatch(fx, {"resume", "validate", std::to_string(task)});
  CHECK(green_text.code == 0);
  CHECK(green_text.out == std::format("OK task:{} is resume-ready\n", task));
}

TEST_CASE("resume validate: a DONE task is still resumable", "[cmd][handlers][resume]") {
  auto const fx   = make_fixture("rvdone");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "snapshot", "--task", std::to_string(task), "--note", "cp"}).code == 0);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute(std::format("update tasks set status = 'done' where id = {}", task)).has_value());

  // Status is NOT a resumability rule. Oracle-probed by driving a task to
  // `done` and re-validating.
  auto const got = dispatch(fx, {"resume", "validate", std::to_string(task), "--json"});
  CHECK(got.code == 0);
  CHECK(got.out == std::format("{{\"task_id\":{},\"resumable\":true,\"failures\":null}}\n", task));
}

TEST_CASE("the resume PACKET is not ported and says so with exit 64", "[cmd][handlers][resume]") {
  auto const fx = make_fixture("rvpacket");
  // Registered on purpose: an UNregistered dual node would fall to
  // dispatch's help path and exit 0, a silent success where the oracle
  // produces a packet. See `planar.cmd.planar.handlers.resume`.
  auto const bare = dispatch(fx, {"resume"});
  CHECK(bare.code == 64);
  CHECK(bare.err == "error: not implemented yet\n");

  auto const with_id = dispatch(fx, {"resume", "2", "--json"});
  CHECK(with_id.code == 64);
}

TEST_CASE("handoff refuses without an active session and does NOT create one", "[cmd][handlers][handoff]") {
  auto const fx  = make_fixture("honosession");
  auto const got = dispatch(fx, {"handoff"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: no active session (run `planar capture session` first)\n");

  // The refusal is the contract (plan 314 task 2296). Auto-starting a
  // session here would mask it — so assert no session row was written.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select count(*) from sessions");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);
}

TEST_CASE("handoff runs the four-step ritual and reports resumability", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("horitual");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);
  REQUIRE(dispatch(fx, {"capture", "snapshot", "--task", std::to_string(task), "--note", "cp"}).code == 0);

  auto const got = dispatch(fx, {"handoff", std::to_string(task), "--json"});
  CHECK(got.code == 0);
  // `"failures":[]` — NOT `null`, which is what `resume validate` emits for
  // the same state. Two renderers, deliberately different.
  CHECK(got.out == "{\"ok\":true,\"snapshot_id\":2,\"handoff_id\":1,\"status\":\"validated\","
                   "\"resumable\":true,\"failures\":[]}\n");

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  // Step 4's session note landed.
  auto stmt = conn->prepare("select count(*) from session_entries where body = 'handoff captured: snapshot=2 handoff=1'");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 1);
}

TEST_CASE("handoff on a NON-resumable task still succeeds and still exits 0", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("honotresumable");
  auto const task = seed_task(fx, "No action", std::nullopt);
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  // The resumability check here is ADVISORY. Only `resume validate` turns
  // it into a non-zero exit.
  auto const got = dispatch(fx, {"handoff", std::to_string(task), "--json"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("\"resumable\":false"));
  CHECK(got.out.contains("\"check\":\"next_action\""));
  CHECK(got.out.contains("\"status\":\"validated\""));

  auto const text = dispatch(fx, {"handoff", std::to_string(task)});
  CHECK(text.code == 0);
  CHECK(text.out.contains("  validate: FAIL \xe2\x80\x94 task is not resume-ready\n"));
}

TEST_CASE("handoff copies worktree context off the ACTIVE claim", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("howorktree");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  // A RELEASED claim must be ignored and an ACTIVE one used, which is the
  // whole point of the status filter in `resolve_worktree_for_task`.
  REQUIRE(conn->execute(std::format("insert into agent_work_claims "
                                    "(claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, "
                                    " worktree_path, repo_root, branch, lease_expires_at) "
                                    "values ('a0', 1, 'task', {}, 'exclusive', 'released', 'cli', "
                                    "        '/tmp/old', '/tmp/oldrepo', 'old', '2030-01-01T00:00:00.000Z')",
                                    task))
              .has_value());
  REQUIRE(conn->execute(std::format("insert into agent_work_claims "
                                    "(claim_token, session_id, entity_kind, entity_id, claim_scope, status, vendor, "
                                    " worktree_path, repo_root, branch, lease_expires_at) "
                                    "values ('a1', 1, 'task', {}, 'exclusive', 'active', 'cli', "
                                    "        '/tmp/wt', '/tmp/repo', 'feature/x', '2030-01-01T00:00:00.000Z')",
                                    task))
              .has_value());

  REQUIRE(dispatch(fx, {"handoff", std::to_string(task), "--json"}).code == 0);

  auto const shown = dispatch(fx, {"handoff", "show", "1", "--json"});
  CHECK(shown.code == 0);
  CHECK(shown.out.contains("\"worktree_path\":\"/tmp/wt\""));
  CHECK(shown.out.contains("\"repo_root\":\"/tmp/repo\""));
  CHECK(shown.out.contains("\"branch\":\"feature/x\""));
  CHECK_FALSE(shown.out.contains("/tmp/old"));
}

TEST_CASE("handoff list defaults to PENDING, which can be zero bytes", "[cmd][handlers][handoff][terminator]") {
  auto const fx   = make_fixture("holist");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);

  auto const empty_json = dispatch(fx, {"handoff", "list", "--json"});
  CHECK(empty_json.code == 0);
  CHECK(empty_json.out.empty());
  auto const empty_text = dispatch(fx, {"handoff", "list"});
  CHECK(empty_text.out == "no handoffs\n");

  // The composite leaves the handoff VALIDATED, so the default filter still
  // finds nothing even though a handoff now exists. This is the trap.
  REQUIRE(dispatch(fx, {"handoff", std::to_string(task), "--json"}).code == 0);
  auto const still_empty = dispatch(fx, {"handoff", "list", "--json"});
  CHECK(still_empty.code == 0);
  CHECK(still_empty.out.empty());

  auto const validated = dispatch(fx, {"handoff", "list", "--status", "validated"});
  CHECK(validated.code == 0);
  CHECK(validated.out == "id    snapshot  from-vendor  to-vendor    status\n"
                         "1     1         cli          -            validated\n");
}

TEST_CASE("handoff list --status parses a comma list and refuses an unknown token", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("holiststatus");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);
  REQUIRE(dispatch(fx, {"handoff", std::to_string(task), "--json"}).code == 0);

  // Spaces around tokens are trimmed; empty tokens are skipped.
  auto const multi = dispatch(fx, {"handoff", "list", "--status", "pending, validated,", "--json"});
  CHECK(multi.code == 0);
  CHECK(std::ranges::count(multi.out, '\n') == 1);

  auto const bad = dispatch(fx, {"handoff", "list", "--status", "bogus", "--json"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: unknown handoff status 'bogus'\n");
}

TEST_CASE("handoff lifecycle verbs and their terminal refusals", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("holifecycle");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);
  REQUIRE(dispatch(fx, {"handoff", std::to_string(task), "--json"}).code == 0);

  auto const consumed = dispatch(fx, {"handoff", "consume", "1", "--session", "1", "--json"});
  CHECK(consumed.code == 0);
  CHECK(consumed.out.contains("\"status\":\"consumed\""));
  CHECK(consumed.out.contains("\"to_session_id\":1"));

  // Each terminal refusal has its OWN wording, and they are not
  // interchangeable.
  auto const revalidate = dispatch(fx, {"handoff", "validate", "1", "--json"});
  CHECK(revalidate.code == 1);
  CHECK(revalidate.err == "error: handoff 1 cannot transition to validated\n");

  auto const abandoning = dispatch(fx, {"handoff", "abandon", "1", "--json"});
  CHECK(abandoning.code == 1);
  CHECK(abandoning.err == "error: handoff 1 is terminal; cannot abandon\n");

  // BUT re-consuming a CONSUMED handoff SUCCEEDS, and that is not a bug in
  // this port — it is the matrix's identity shortcut (`from == to` returns
  // success before the per-kind switch runs), and the UPDATE then re-stamps
  // `consumed_at`. Verified against the oracle directly: `handoff consume 1`
  // on an already-consumed handoff exits 0 there too. `terminal` therefore
  // means "no edges to a DIFFERENT status", not "frozen".
  auto const reconsume = dispatch(fx, {"handoff", "consume", "1", "--json"});
  CHECK(reconsume.code == 0);
  CHECK(reconsume.err.empty());
  CHECK(reconsume.out.contains("\"status\":\"consumed\""));

  // And the same shortcut on the other terminal: re-abandoning succeeds.
  REQUIRE(dispatch(fx, {"handoff", "create", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"handoff", "abandon", "2", "--json"}).code == 0);
  auto const reabandon = dispatch(fx, {"handoff", "abandon", "2", "--json"});
  CHECK(reabandon.code == 0);
  CHECK(reabandon.out.contains("\"status\":\"abandoned\""));
}

TEST_CASE("handoff create anchors on an existing snapshot", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("hocreate");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "snapshot", "--task", std::to_string(task), "--note", "cp"}).code == 0);

  auto const absent = dispatch(fx, {"handoff", "create", "99", "--json"});
  CHECK(absent.code == 1);
  CHECK(absent.err == "error: snapshot 99 not found\n");

  auto const created = dispatch(fx, {"handoff", "create", "1", "--vendor", "codex", "--json"});
  CHECK(created.code == 0);
  // `create` leaves the handoff PENDING — it is the escape hatch, not the
  // ritual, so it does not validate.
  CHECK(created.out.contains("\"status\":\"pending\""));
  CHECK(created.out.contains("\"to_vendor\":\"codex\""));
}

TEST_CASE("handoff show and the id-parse refusals", "[cmd][handlers][handoff]") {
  auto const fx = make_fixture("hoshow");

  auto const absent = dispatch(fx, {"handoff", "show", "1", "--json"});
  CHECK(absent.code == 1);
  CHECK(absent.err == "error: handoff 1 not found\n");

  auto const bad = dispatch(fx, {"handoff", "show", "abc"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: handoff id must be an integer, got 'abc'\n");

  // Zig `parseInt` accepts underscore separators, so `1_0` is 10 — the
  // single `parse_int64_zig` in `planar.cliapp.args` is what preserves it.
  auto const separators = dispatch(fx, {"handoff", "show", "1_0"});
  CHECK(separators.code == 1);
  CHECK(separators.err == "error: handoff 10 not found\n");
}

TEST_CASE("handoff abandon accepts --reason and stores nothing for it", "[cmd][handlers][handoff]") {
  auto const fx   = make_fixture("hoabandon");
  auto const task = seed_task(fx, "Has action", "do the thing");
  REQUIRE(dispatch(fx, {"capture", "session"}).code == 0);
  REQUIRE(dispatch(fx, {"handoff", std::to_string(task), "--json"}).code == 0);

  auto const abandoned = dispatch(fx, {"handoff", "abandon", "1", "--reason", "superseded", "--json"});
  CHECK(abandoned.code == 0);
  CHECK(abandoned.out.contains("\"status\":\"abandoned\""));
  // The reason reaches the audit summary in the oracle and NO column here;
  // `validated_at` survives the transition either way.
  CHECK(abandoned.out.contains("\"validated_at\""));
  CHECK_FALSE(abandoned.out.contains("superseded"));
  CHECK_FALSE(abandoned.out.contains("consumed_at"));
}

// ---------------------------------------------------------------------------
// `init` (plan 996, task 6132)
//
// EVERY CASE BELOW ASSERTS ON THE DATABASE, NOT ONLY ON STDOUT. That is the
// whole point of this block. Task 6128 is the standing counterexample in this
// tree: `planar capture session` exits 0 with stdout byte-identical to the
// oracle while writing NULL `repo_root` and NULL `head_sha_at_start`, because
// the engine supports the fields and the handler never passes them. Every
// other port gap in this tree announces itself with exit 64; that one looks
// exactly like success, and a stdout-only test suite cannot see it.
//
// `projects.git_remote` has precisely that shape for `init`, and it is
// invisible in the common fixture: a scratch directory has no `origin`, so a
// handler that never probed git at all would still match the oracle
// byte-for-byte everywhere except a fixture that runs `git init` and
// `git remote add` first. `init writes the git remote` below is that fixture.
//
// ORACLE PROVENANCE. Every expected string is bytes the Zig reference binary
// wrote under a pinned scratch PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/
// PLANAR_LOCAL_HOME/HOME, read back through `cat -e`:
//
//   $Z init
//     exit 0, stdout b'planar initialized\n  db:      <db>\n  schema:  33\n
//                      project: proj (id: 1)\n
//                      next:    `planar assoc create project:proj --kind project`\n
//                              `planar assoc add project:proj <root>`\n'
//   $Z init --json
//     exit 0, b'{"ok":true,"db":"<db>","schema_version":33,"project_id":1,
//                "project_slug":"proj","project_name":"proj","root_path":"<root>"}\n'
//   $Z init --json          (cwd is a git repo with an `origin` remote)
//     …same, plus b',"git_remote":"git@github.com:example/repo.git"' before the brace
//   $Z init --json          (cwd is a git repo whose ONLY remote is `upstream`)
//     …NO "git_remote" key at all
//   $Z init --skip-project --json
//     exit 0, b'{"ok":true,"db":"<db>","schema_version":33}\n'   <- five keys ABSENT
//   $Z init --skip-project
//     exit 0, b'planar initialized\n  db:      <db>\n  schema:  33\n'
//   $Z init --allow-no-repo (in a NON-git directory)
//     exit 0, byte-identical to plain `init` — the flag is declared and never read

namespace {

/// @brief Read one `projects` row back, or report that there is none.
/// @param fx The fixture whose database to read.
/// @return `(id, slug, name, root_path, git_remote, remote_is_null)` for the
/// single row, or unset when the table is empty.
struct project_row {
  std::int64_t id = 0;                ///< The row id.
  std::string  slug;                  ///< The slug column.
  std::string  name;                  ///< The name column.
  std::string  root_path;             ///< The root_path column (empty when NULL).
  std::string  git_remote;            ///< The git_remote column (empty when NULL).
  bool         remote_is_null = true; ///< Whether git_remote is SQL NULL, as distinct from the empty string.
};

/// @brief Read the sole `projects` row from `fx`'s database.
/// @param fx The fixture.
/// @return The row, or unset when `projects` is empty.
auto read_project(const fixture& fx) -> std::optional<project_row> {
  auto conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return std::nullopt;
  }
  auto stmt = conn->prepare("select id, slug, name, root_path, git_remote from projects order by id");
  if (!stmt) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return std::nullopt;
  }
  return project_row{
      .id             = stmt->column_int64(0),
      .slug           = stmt->column_text(1),
      .name           = stmt->column_text(2),
      .root_path      = stmt->is_null(3) ? std::string{} : stmt->column_text(3),
      .git_remote     = stmt->is_null(4) ? std::string{} : stmt->column_text(4),
      .remote_is_null = stmt->is_null(4),
  };
}

/// @brief The database's applied schema version.
/// @param fx The fixture.
/// @return `max(version)` from `schema_migrations`, or 0.
auto read_schema_version(const fixture& fx) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return 0;
  }
  auto stmt = conn->prepare("select coalesce(max(version), 0) from schema_migrations");
  if (!stmt) {
    return 0;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return 0;
  }
  return stmt->column_int64(0);
}

/// @brief How many tables the database carries — the coarse "migrations
/// actually ran" signal, independent of what `schema_migrations` claims
/// about itself.
/// @param fx The fixture.
/// @return The table count.
auto read_table_count(const fixture& fx) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return 0;
  }
  auto stmt = conn->prepare("select count(*) from sqlite_master where type = 'table'");
  if (!stmt) {
    return 0;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return 0;
  }
  return stmt->column_int64(0);
}

/// @brief Make `fx`'s working directory a git repository with `remote_name`
/// pointing at `url`.
/// @param fx The fixture.
/// @param remote_name The remote to add.
/// @param url The URL to give it.
/// @return `true` when git accepted every step.
auto seed_git_remote(const fixture& fx, std::string_view remote_name, std::string_view url) -> bool {
  auto const dir  = (fx.root / "proj").string();
  auto const line = std::format("git -C '{}' init -q . >/dev/null 2>&1 && git -C '{}' remote add {} '{}' >/dev/null 2>&1", dir,
                                dir, remote_name, url);
  return std::system(line.c_str()) == 0;
}

} // namespace

TEST_CASE("init creates the database, applies every migration, and registers cwd", "[cmd][handlers][init]") {
  auto const fx = make_fixture("initplain");
  REQUIRE_FALSE(std::filesystem::exists(fx.db_path));

  auto const got = dispatch(fx, {"init"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.db_open);

  // --- the DATABASE, which is the contract stdout only summarises ---
  REQUIRE(std::filesystem::exists(fx.db_path));
  auto const version = read_schema_version(fx);
  CHECK(version == 33);
  // Not just "some migrations ran": the real schema carries ~90 tables, so a
  // partially-applied chain cannot pass this by having written a
  // `schema_migrations` row.
  CHECK(read_table_count(fx) > 80);

  auto const row = read_project(fx);
  REQUIRE(row.has_value());
  CHECK(row->id == 1);
  CHECK(row->slug == "proj");
  CHECK(row->name == "proj");
  CHECK(row->root_path == (fx.root / "proj").string());
  // No repository in the fixture, so NULL — and NULL specifically, not "".
  CHECK(row->remote_is_null);

  // --- and the bytes ---
  CHECK(got.out == std::format("planar initialized\n"
                               "  db:      {}\n"
                               "  schema:  {}\n"
                               "  project: proj (id: 1)\n"
                               "  next:    `planar assoc create project:proj --kind project`\n"
                               "           `planar assoc add project:proj {}`\n",
                               fx.db_path.string(), version, (fx.root / "proj").string()));
}

TEST_CASE("init --json omits git_remote rather than emitting null", "[cmd][handlers][init][parity]") {
  auto const fx  = make_fixture("initjson");
  auto const got = dispatch(fx, {"init", "--json"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == std::format(R"({{"ok":true,"db":"{}","schema_version":{},"project_id":1,)"
                               R"("project_slug":"proj","project_name":"proj","root_path":"{}"}})"
                               "\n",
                               fx.db_path.string(), read_schema_version(fx), (fx.root / "proj").string()));
  CHECK_FALSE(got.out.contains("git_remote"));
  CHECK_FALSE(got.out.contains("null"));
}

TEST_CASE("init writes the git remote into the row and the JSON", "[cmd][handlers][init][6128]") {
  // THE 6128 GUARD. Everything else about `init` is identical whether or not
  // the handler probes git at all; this case is the only one that can tell
  // the difference, and it asserts on the COLUMN first.
  auto const fx = make_fixture("initremote");
  if (!seed_git_remote(fx, "origin", "git@github.com:example/repo.git")) {
    SKIP("git unavailable — the remote-capture arm cannot be exercised");
  }

  auto const got = dispatch(fx, {"init", "--json"});
  CHECK(got.code == 0);

  auto const row = read_project(fx);
  REQUIRE(row.has_value());
  CHECK_FALSE(row->remote_is_null);
  CHECK(row->git_remote == "git@github.com:example/repo.git");

  CHECK(got.out.ends_with(R"(,"git_remote":"git@github.com:example/repo.git"})"
                          "\n"));
}

TEST_CASE("init captures origin and only origin", "[cmd][handlers][init][6128]") {
  // `upstream` alone is NOT a remote as far as `init` is concerned: the
  // oracle runs `git remote get-url origin` specifically, so a handler that
  // took "the first remote" or "any remote" would store a URL the oracle
  // leaves NULL.
  auto const fx = make_fixture("initupstream");
  if (!seed_git_remote(fx, "upstream", "https://example.com/up.git")) {
    SKIP("git unavailable — the remote-capture arm cannot be exercised");
  }

  auto const got = dispatch(fx, {"init", "--json"});
  CHECK(got.code == 0);
  CHECK_FALSE(got.out.contains("git_remote"));
  CHECK_FALSE(got.out.contains("up.git"));

  auto const row = read_project(fx);
  REQUIRE(row.has_value());
  CHECK(row->remote_is_null);
}

TEST_CASE("init --skip-project migrates the database and registers nothing", "[cmd][handlers][init]") {
  auto const fx  = make_fixture("initskip");
  auto const got = dispatch(fx, {"init", "--skip-project"});
  CHECK(got.code == 0);
  CHECK(got.out == std::format("planar initialized\n"
                               "  db:      {}\n"
                               "  schema:  {}\n",
                               fx.db_path.string(), read_schema_version(fx)));

  // Migrated — but no row. Both halves matter: a handler that skipped the
  // whole verb would also leave `projects` empty.
  CHECK(read_schema_version(fx) == 33);
  CHECK(read_table_count(fx) > 80);
  CHECK_FALSE(read_project(fx).has_value());
}

TEST_CASE("init --skip-project --json emits exactly two keys beyond ok", "[cmd][handlers][init][parity]") {
  auto const fx  = make_fixture("initskipj");
  auto const got = dispatch(fx, {"init", "--skip-project", "--json"});
  CHECK(got.code == 0);
  CHECK(got.out == std::format(R"({{"ok":true,"db":"{}","schema_version":{}}})"
                               "\n",
                               fx.db_path.string(), read_schema_version(fx)));
  // All five project keys omitted, not nulled.
  CHECK_FALSE(got.out.contains("project_id"));
  CHECK_FALSE(got.out.contains("project_slug"));
  CHECK_FALSE(got.out.contains("project_name"));
  CHECK_FALSE(got.out.contains("root_path"));
  CHECK_FALSE(got.out.contains("git_remote"));
}

TEST_CASE("init honours --name and --slug", "[cmd][handlers][init]") {
  auto const fx  = make_fixture("initnameslug");
  auto const got = dispatch(fx, {"init", "--name", "My Proj", "--slug", "custom-slug", "--json"});
  CHECK(got.code == 0);

  auto const row = read_project(fx);
  REQUIRE(row.has_value());
  CHECK(row->slug == "custom-slug");
  CHECK(row->name == "My Proj");
  CHECK(got.out.contains(R"("project_slug":"custom-slug")"));
  CHECK(got.out.contains(R"("project_name":"My Proj")"));
}

TEST_CASE("init is idempotent and --force repoints the same row", "[cmd][handlers][init]") {
  auto const fx = make_fixture("initidem");
  REQUIRE(dispatch(fx, {"init"}).code == 0);
  auto const first = read_project(fx);
  REQUIRE(first.has_value());

  // Second run: INSERT OR IGNORE, so `--name` is NOT applied and the row is
  // untouched. A handler that used plain INSERT would fail here on the unique
  // slug; one that used upsert unconditionally would rename the project.
  auto const again = dispatch(fx, {"init", "--name", "Ignored"});
  CHECK(again.code == 0);
  auto const second = read_project(fx);
  REQUIRE(second.has_value());
  CHECK(second->id == first->id);
  CHECK(second->name == first->name);
  CHECK(second->name != "Ignored");

  // With --force the SAME row id is repointed rather than a new one inserted.
  auto const forced = dispatch(fx, {"init", "--force", "--name", "Renamed"});
  CHECK(forced.code == 0);
  auto const third = read_project(fx);
  REQUIRE(third.has_value());
  CHECK(third->id == first->id);
  CHECK(third->name == "Renamed");
}

TEST_CASE("init --allow-no-repo is accepted and changes nothing", "[cmd][handlers][init]") {
  // Probed against the oracle rather than inferred from the flag's help text:
  // `init` in a non-git directory WITHOUT the flag already succeeds, and the
  // oracle's handler never reads the flag. Reproducing the no-op is D2;
  // enforcing what the help text implies would refuse what the oracle accepts.
  auto const bare    = make_fixture("initbare");
  auto const flagged = make_fixture("initanr");

  auto const without = dispatch(bare, {"init", "--json"});
  auto const with    = dispatch(flagged, {"init", "--allow-no-repo", "--json"});
  CHECK(without.code == 0);
  CHECK(with.code == 0);

  // Identical apart from the two scratch paths each echoes.
  auto const normalise = [](std::string text, const std::filesystem::path& root) {
    auto const needle = root.string();
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 4)) {
      text.replace(at, needle.size(), "ROOT");
    }
    return text;
  };
  CHECK(normalise(without.out, bare.root) == normalise(with.out, flagged.root));

  auto const row = read_project(flagged);
  REQUIRE(row.has_value());
  CHECK(row->slug == "proj");
}

// =========================================================================
// `plan create` and `assoc create` (task 6133).
//
// EVERY case below asserts the resulting DATABASE ROW, not just the exit
// code and stdout. That is not belt-and-braces: it is the specific gap
// that let task 6128 ship `capture session` writing NULL columns behind
// byte-identical stdout, and task 6132 ship `init` registering the wrong
// `projects.root_path` behind an exit 0. Both engines here take OPTIONAL
// arguments that default silently when a handler omits them, so a
// stdout-only assertion cannot tell a threaded value from a defaulted one.
//
// Oracle fixture and capture (scratch PLANAR_DB/PLANAR_HOME/HOME, cwd =
// <root>/proj, `zig/zig-out/bin/planar`):
//   $Z assoc create my-assoc
//     b'id:        1\nslug:      my-assoc\nname:      my-assoc\n
//       kind:      ad-hoc\nauto:      no\ncreated:   <ts>\nupdated:   <ts>\n'
//   $Z assoc create other --name "Other Name" --kind org --json
//     b'{"id":2,"slug":"other","name":"Other Name","kind":"org",
//       "auto_detected":false,"config_json":null,...}\n'
//   $Z assoc create dup-x ; $Z assoc create dup-x
//     -> exit 6, b'error: association create: SlugConflict\n'
//   $Z assoc create badkind --kind nope
//     -> exit 2, b"error: unknown kind 'nope'\n"
//   $Z init ; $Z plan create "Unassoc Plan"
//     -> exit 5, b'error: plan create: project has no association; run
//        `planar assoc create project:proj --kind project` then `planar
//        assoc add project:proj <repo-path>`, or pass `--scope global`
//        explicitly\n'
//   (after `assoc add my-assoc <cwd>`) $Z plan create "Now Associated"
//     b'id:       1\ntitle:    Now Associated\nslug:     now-associated\n
//       status:   draft\nscope:    association:1\ncreated: ...\nupdated: ...\n'
//   $Z plan create "With Summary" --summary "some text" --status active
//        --slug custom-slug
//     -> the `summary:` line appears AFTER `scope:`, and `parent:` before it
//   $Z plan create "Bad Status" --status bogus
//     -> exit 1 (NOT 2), b"error: unknown status 'bogus'\n"
// =========================================================================

namespace {

/// @brief One `associations` row, read straight out of SQLite.
struct assoc_row {
  std::int64_t id = 0;         ///< The row id.
  std::string  slug;           ///< The slug column.
  std::string  name;           ///< The name column.
  std::string  kind;           ///< The kind column.
  std::int64_t auto_detected;  ///< The auto_detected column.
  bool         config_is_null; ///< Whether config_json is NULL.
};

/// @brief Read every `associations` row, ordered by id.
/// @param fx The fixture.
/// @return The rows.
auto read_assocs(const fixture& fx) -> std::vector<assoc_row> {
  std::vector<assoc_row> rows;
  auto                   conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return rows;
  }
  auto stmt = conn->prepare("select id, slug, name, kind, auto_detected, config_json from associations order by id");
  if (!stmt) {
    return rows;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped != planar::db::step_result::row) {
      return rows;
    }
    rows.push_back(assoc_row{
        .id             = stmt->column_int64(0),
        .slug           = stmt->column_text(1),
        .name           = stmt->column_text(2),
        .kind           = stmt->column_text(3),
        .auto_detected  = stmt->column_int64(4),
        .config_is_null = stmt->is_null(5),
    });
  }
}

/// @brief One `plans` row, read straight out of SQLite.
struct plan_row {
  std::int64_t id = 0;         ///< The row id.
  std::string  scope_kind;     ///< The scope_kind column.
  std::string  scope_id;       ///< The scope_id column, or "~" when NULL.
  std::string  title;          ///< The title column.
  std::string  slug;           ///< The slug column.
  std::string  summary;        ///< The summary column, or "~" when NULL.
  std::string  status;         ///< The status column.
  std::string  parent_plan_id; ///< The parent_plan_id column, or "~" when NULL.
};

/// @brief Read every `plans` row, ordered by id.
///
/// NULLs come back as the sentinel `"~"` rather than an empty string so a
/// case can tell "the column was never written" from "the column was
/// written empty" — the exact distinction task 6128's NULL columns turned
/// on.
/// @param fx The fixture.
/// @return The rows.
auto read_plans(const fixture& fx) -> std::vector<plan_row> {
  std::vector<plan_row> rows;
  auto                  conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return rows;
  }
  auto stmt = conn->prepare("select id, scope_kind, scope_id, title, slug, summary, status, parent_plan_id "
                            "from plans order by id");
  if (!stmt) {
    return rows;
  }
  auto const col = [&stmt](int i) -> std::string { return stmt->is_null(i) ? std::string{"~"} : stmt->column_text(i); };
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped != planar::db::step_result::row) {
      return rows;
    }
    rows.push_back(plan_row{
        .id             = stmt->column_int64(0),
        .scope_kind     = stmt->column_text(1),
        .scope_id       = col(2),
        .title          = stmt->column_text(3),
        .slug           = stmt->column_text(4),
        .summary        = col(5),
        .status         = stmt->column_text(6),
        .parent_plan_id = col(7),
    });
  }
}

} // namespace

TEST_CASE("assoc create defaults name to the slug and kind to ad-hoc, in the ROW", "[cmd][handlers][assoc][parity][6133]") {
  auto const fx  = make_fixture("acdefault");
  auto const got = dispatch(fx, {"assoc", "create", "my-assoc"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  // Ten-column label padding, wider than `plan create`'s nine. `config:`
  // is absent because `config_json` is NULL — the conditional line.
  CHECK(got.out.starts_with("id:        1\n"
                            "slug:      my-assoc\n"
                            "name:      my-assoc\n"
                            "kind:      ad-hoc\n"
                            "auto:      no\n"
                            "created:   "));
  CHECK(got.out.contains("\nupdated:   "));
  CHECK_FALSE(got.out.contains("config:"));
  CHECK(got.out.ends_with("\n"));

  auto const rows = read_assocs(fx);
  REQUIRE(rows.size() == 1);
  // The defaults land in the ROW, not merely in the rendered line. An
  // absent `--kind` must never reach the unknown-kind refusal, and the
  // name must default to the slug rather than to the empty string.
  CHECK(rows[0].slug == "my-assoc");
  CHECK(rows[0].name == "my-assoc");
  CHECK(rows[0].kind == "ad-hoc");
  CHECK(rows[0].auto_detected == 0); // operator-created, never auto-detected
  CHECK(rows[0].config_is_null);
}

TEST_CASE("assoc create threads --name and --kind into the row and the JSON", "[cmd][handlers][assoc][parity][6133]") {
  auto const fx  = make_fixture("acflags");
  auto const got = dispatch(fx, {"assoc", "create", "other", "--name", "Other Name", "--kind", "org", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with(R"({"id":1,"slug":"other","name":"Other Name","kind":"org",)"
                            R"("auto_detected":false,"config_json":null,)"));
  // JSON is a fragment the handler terminates; text carries its own.
  CHECK(got.out.ends_with("}\n"));

  auto const rows = read_assocs(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].name == "Other Name");
  CHECK(rows[0].kind == "org");
}

TEST_CASE("assoc create refuses a duplicate slug at exit 6 without writing", "[cmd][handlers][assoc][parity][6133]") {
  auto const fx = make_fixture("acdup");
  REQUIRE(dispatch(fx, {"assoc", "create", "dup-x"}).code == 0);

  auto const again = dispatch(fx, {"assoc", "create", "dup-x"});
  CHECK(again.code == 6);
  // "association create", not the "assoc create" the operator typed — the
  // oracle's message spells the engine module's name.
  CHECK(again.err == "error: association create: SlugConflict\n");
  CHECK(again.out.empty());
  CHECK(read_assocs(fx).size() == 1);
}

TEST_CASE("assoc create refuses an unknown --kind at exit 2 BEFORE writing", "[cmd][handlers][assoc][parity][6133]") {
  auto const fx  = make_fixture("ackind");
  auto const got = dispatch(fx, {"assoc", "create", "badkind", "--kind", "nope"});
  CHECK(got.code == 2); // 2 here; `plan create`'s bad --status is 1. Not a typo.
  CHECK(got.err == "error: unknown kind 'nope'\n");
  CHECK(got.out.empty());
  // The refusal precedes the write: no half-created row survives it.
  CHECK(read_assocs(fx).empty());
}

TEST_CASE("plan create REFUSES an unassociated project instead of filing under global", "[cmd][handlers][plan][parity][6133]") {
  // THE case this verb exists to get right. `create_plan`'s `scope` is
  // optional and writes `global` when unset, so a handler that threaded
  // `--scope` straight through would exit 0 here, print a plausible row,
  // and silently file the plan under `global` — the task-6128/6132 shape.
  // The oracle refuses at exit 5 and names both remedy commands.
  auto const fx = make_fixture("pcunassoc");
  REQUIRE(dispatch(fx, {"init"}).code == 0);

  auto const got = dispatch(fx, {"plan", "create", "Unassoc Plan"});
  CHECK(got.code == 5);
  CHECK(got.out.empty());
  CHECK(got.err == "error: plan create: project has no association; run `planar assoc create project:proj --kind project` "
                   "then `planar assoc add project:proj <repo-path>`, or pass `--scope global` explicitly\n");
  // A refusal, NOT a fallback: nothing was written.
  CHECK(read_plans(fx).empty());
}

TEST_CASE("plan create stores the cwd-derived association scope in the ROW", "[cmd][handlers][plan][parity][6133]") {
  // The positive half of the same invariant. With the cwd joined to an
  // association, `association:1` is something ONLY the cwd derivation can
  // produce — deleting `resolve_write_scope` flips this to `global`.
  auto const fx = make_fixture("pcassoc");
  REQUIRE(dispatch(fx, {"init"}).code == 0);
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    REQUIRE(planar::engine::identity::create(**conn, {.slug = "alpha"}).has_value());
    REQUIRE(planar::engine::identity::add_member(**conn, "alpha", (fx.root / "proj").string()).has_value());
  }

  auto const got = dispatch(fx, {"plan", "create", "Now Associated"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with("id:       1\n"
                            "title:    Now Associated\n"
                            "slug:     now-associated\n"
                            "status:   draft\n"
                            "scope:    association:1\n"));

  auto const rows = read_plans(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].scope_kind == "association");
  CHECK(rows[0].scope_id == "1");
  CHECK(rows[0].slug == "now-associated"); // slug derived from the title
  CHECK(rows[0].status == "draft");
  CHECK(rows[0].summary == "~");
  CHECK(rows[0].parent_plan_id == "~");
}

TEST_CASE("plan create threads EVERY optional flag into the row", "[cmd][handlers][plan][parity][6133]") {
  // The anti-defaulting case. `plan_create_args` has five optional
  // members; a handler that dropped any one of them still compiles, still
  // exits 0, and still prints a row that looks right at a glance. Each
  // assertion below fails on exactly one dropped argument.
  auto const fx = make_fixture("pcflags");
  REQUIRE(dispatch(fx, {"plan", "create", "Parent Plan", "--scope", "global"}).code == 0);

  auto const got = dispatch(fx, {"plan", "create", "With Everything", "--scope", "global", "--summary", "some text", "--slug",
                                 "custom-slug", "--status", "active", "--parent", "1"});
  REQUIRE(got.code == 0);
  // `parent:` BEFORE `summary:`, which is not the struct's field order.
  CHECK(got.out.starts_with("id:       2\n"
                            "title:    With Everything\n"
                            "slug:     custom-slug\n"
                            "status:   active\n"
                            "scope:    global\n"
                            "parent:   1\n"
                            "summary:  some text\n"
                            "created:  "));

  auto const rows = read_plans(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[1].title == "With Everything");
  CHECK(rows[1].slug == "custom-slug"); // --slug, not the title-derived slug
  CHECK(rows[1].summary == "some text");
  CHECK(rows[1].status == "active");
  CHECK(rows[1].parent_plan_id == "1");
  CHECK(rows[1].scope_kind == "global");
  CHECK(rows[1].scope_id == "~");
}

TEST_CASE("plan create --scope wins over the cwd and reaches the row", "[cmd][handlers][plan][parity][6133]") {
  auto const fx = make_fixture("pcscope");
  REQUIRE(dispatch(fx, {"init"}).code == 0);
  {
    std::ostringstream out;
    std::ostringstream err;
    context            ctx{{"planar"}, planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
    auto               conn = ctx.ensure_db();
    REQUIRE(conn.has_value());
    REQUIRE(planar::engine::identity::create(**conn, {.slug = "alpha"}).has_value());
    REQUIRE(planar::engine::identity::add_member(**conn, "alpha", (fx.root / "proj").string()).has_value());
  }

  // The cwd would derive `association:1`; each explicit flag overrides it.
  REQUIRE(dispatch(fx, {"plan", "create", "Repo Scoped", "--scope", "repo:proj"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Global Explicit", "--scope", "global"}).code == 0);

  auto const rows = read_plans(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].scope_kind == "repo");
  CHECK(rows[0].scope_id == "1");
  CHECK(rows[1].scope_kind == "global");
  CHECK(rows[1].scope_id == "~");
}

TEST_CASE("plan create refuses a bad --status at exit 1, and a bad --scope at exit 1", "[cmd][handlers][plan][parity][6133]") {
  auto const fx = make_fixture("pcbad");

  auto const status = dispatch(fx, {"plan", "create", "Bad Status", "--status", "bogus"});
  // ONE, not two. zig dies with `error.InvalidStatus`, which has no arm in
  // `codeFor` — unlike `assoc create`'s `--kind`, which dies with
  // `error.InvalidInput` and maps to 2. Oracle-captured on both sides.
  CHECK(status.code == 1);
  CHECK(status.err == "error: unknown status 'bogus'\n");
  // The database IS opened and migrated before the refusal — zig's handler
  // calls `ensureDb` first and only then parses `--status`. Oracle-
  // confirmed against an empty scratch root, which came away with a
  // migrated `planar.db`. Asserted positively so a future "validate
  // arguments first" tidy-up cannot silently change when migration runs.
  CHECK(status.db_open);

  auto const scope = dispatch(fx, {"plan", "create", "Bad Scope", "--scope", "nonexistent-scope"});
  CHECK(scope.code == 1);
  CHECK(scope.err == "error: plan create: SlugNotFound\n");

  CHECK(read_plans(fx).empty());
}

TEST_CASE("plan create --json emits the struct's field order with a terminator", "[cmd][handlers][plan][parity][6133]") {
  auto const fx  = make_fixture("pcjson");
  auto const got = dispatch(fx, {"plan", "create", "JSON Plan", "--scope", "global", "--summary", "s", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with(R"({"id":1,"scope_kind":"global","scope_id":null,"title":"JSON Plan",)"
                            R"("slug":"json-plan","summary":"s","status":"draft","parent_plan_id":null,)"));
  CHECK(got.out.ends_with("}\n"));
}

// ===========================================================================
// task 6135 — `task add` and `assoc add`
// ===========================================================================
//
// ORACLE PROVENANCE. Every expected byte below was captured by running the
// zig binary and this port over the SAME argv script, each pinned to its own
// scratch root (`cd <dir> && env PLANAR_DB=… HOME=… <binary>` — the
// assignment must follow the `cd`, not precede an `&&`), and diffing
// stdout / stderr / exit code / every `tasks`, `projects` and
// `project_associations` column including `typeof()`. Twenty-six `task add`
// invocations and twenty `assoc add` invocations agree byte for byte and
// column for column. The one surviving log difference is an ORACLE artifact:
// a mangled `d: StepFailed` line on the two `QueryFailed` paths, which is
// `std.log.err("task.create exec failed: {s}")` half-overwritten by zig's
// buffered stderr writer. It is not reproduced here.
//
//   $Z task add "cwd-derived task"            (cwd joined to association 1)
//     exit 0, b'id:          1\ntitle:       cwd-derived task\n
//              status:      todo\npriority:    100\nscope:       association:1\n
//              created:     <ts>\nupdated:     <ts>\n'
//   $Z task add "full task" --body B --next-action NA --due 2026-09-01 \
//               --plan 1 --slug full-task --priority 3 --json
//     exit 0, b'{"id":2,"scope_kind":"association","scope_id":1,"plan_id":1,
//              "parent_task_id":null,"title":"full task","body":"B",
//              "slug":"full-task","status":"todo","priority":3,
//              "next_action":"NA","due_at":"2026-09-01",…}\n'
//   $Z task add "dup slug" --slug full-task   -> exit 6, b'error: task add: SlugConflict\n'
//   $Z task add "bad scope" --scope nosuchscope
//                                             -> exit 1, b'error: task add: SlugNotFound\n'
//   $Z task add "bad plan" --plan 999         -> exit 1, b'error: task add: QueryFailed\n'
//   $Z task add "neg pri" --priority -5       -> exit 0, TEXT prints 'priority:    0'
//   $Z task add "neg pri json" --priority -5 --json -> JSON prints '"priority":-5'
//   $Z task add "due garbage" --due not-a-date-> exit 1, b'error: task add: InvalidDueAt\n'
//   $Z task add "due feb30"  --due 2026-02-30 -> exit 1, same
//   $Z task add "due trailing" --due '2026-09-01T10:00:00Z ' -> exit 1, same
//   $Z task add "due empty" --due ''          -> exit 0, row stores '' (NOT NULL)
//   $Z task add "x" --scope nosuchscope --due garbage -> SlugNotFound, not InvalidDueAt
//   $Z task add "underscore pri" --priority 1_0 -> exit 0, priority 10
//   (in a registered but UNASSOCIATED project) $Z task add "t"
//     -> exit 0, scope_kind='global'.  NO refusal, unlike `plan create`.
//   $Z assoc add acme <path>                  -> exit 0, b'added project at <path> to acme\n'
//   $Z assoc add acme <path> --json
//     -> exit 0, b'{"status":"added","association":"acme","repo_path":"<path>"}\n'
//   $Z assoc add acme <same path again>
//     -> exit 1, b"error: project at '<path>' is already a member of 'acme'\n"
//   $Z assoc add nosuch <path>                -> exit 1, b"error: no association named 'nosuch'\n"
//   $Z assoc add acme /nonexistent/path/xyz   -> exit 0; projects row slug='xyz'
//   $Z assoc add acme '<repo>/'  (TRAILING SLASH)
//     -> exit 0; projects row slug='repo-2', name='repo'
//   $Z assoc add acme /                       -> exit 0; projects row slug='_', name=''
//   $Z assoc add acme '/tmp/pa"th' --json     -> emits INVALID JSON, unescaped

namespace {

/// @brief One `tasks` row, read straight out of SQLite.
///
/// Every optional column comes back as the sentinel `"~"` when NULL, so a
/// case can tell "never written" from "written empty". That distinction is
/// the whole point of these assertions: a handler that drops `--body` on
/// the floor and one that passes `""` both exit 0 and both print output an
/// eyeball accepts.
struct task_row {
  std::int64_t id = 0;         ///< The row id.
  std::string  scope_kind;     ///< The scope_kind column.
  std::string  scope_id;       ///< scope_id, or "~" when NULL.
  std::string  plan_id;        ///< plan_id, or "~" when NULL.
  std::string  parent_task_id; ///< parent_task_id, or "~" when NULL.
  std::string  title;          ///< The title column.
  std::string  body;           ///< body, or "~" when NULL.
  std::string  slug;           ///< slug, or "~" when NULL.
  std::string  status;         ///< The status column.
  std::int64_t priority = 0;   ///< The priority column, UNCLAMPED.
  std::string  next_action;    ///< next_action, or "~" when NULL.
  std::string  due_at;         ///< due_at, or "~" when NULL.
};

/// @brief Read every `tasks` row, ordered by id.
/// @param fx The fixture.
/// @return The rows.
auto read_tasks(const fixture& fx) -> std::vector<task_row> {
  std::vector<task_row> rows;
  auto                  conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return rows;
  }
  auto stmt = conn->prepare("select id, scope_kind, scope_id, plan_id, parent_task_id, title, body, slug, "
                            "status, priority, next_action, due_at from tasks order by id");
  if (!stmt) {
    return rows;
  }
  auto const col = [&stmt](int i) -> std::string { return stmt->is_null(i) ? std::string{"~"} : stmt->column_text(i); };
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped != planar::db::step_result::row) {
      return rows;
    }
    rows.push_back(task_row{
        .id             = stmt->column_int64(0),
        .scope_kind     = stmt->column_text(1),
        .scope_id       = col(2),
        .plan_id        = col(3),
        .parent_task_id = col(4),
        .title          = stmt->column_text(5),
        .body           = col(6),
        .slug           = col(7),
        .status         = stmt->column_text(8),
        .priority       = stmt->column_int64(9),
        .next_action    = col(10),
        .due_at         = col(11),
    });
  }
}

/// @brief Read every `projects` row, ordered by id.
///
/// Reuses `project_row` from the task-6132 block above rather than
/// declaring a second shape for the same table. `name_is_null` is not a
/// member there, so the one case that needs the NULL-vs-`''` distinction
/// on `name` asks `project_name_is_null` directly.
/// @param fx The fixture.
/// @return The rows.
auto read_projects(const fixture& fx) -> std::vector<project_row> {
  std::vector<project_row> rows;
  auto                     conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return rows;
  }
  auto stmt = conn->prepare("select id, slug, name, root_path, git_remote from projects order by id");
  if (!stmt) {
    return rows;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped != planar::db::step_result::row) {
      return rows;
    }
    rows.push_back(project_row{
        .id             = stmt->column_int64(0),
        .slug           = stmt->column_text(1),
        .name           = stmt->column_text(2),
        .root_path      = stmt->is_null(3) ? std::string{} : stmt->column_text(3),
        .git_remote     = stmt->is_null(4) ? std::string{} : stmt->column_text(4),
        .remote_is_null = stmt->is_null(4),
    });
  }
}

/// @brief Whether the first `projects` row's `name` column is SQL NULL, as
/// distinct from the empty string.
/// @param fx The fixture.
/// @return `true` when the column is NULL (or unreadable).
auto project_name_is_null(const fixture& fx) -> bool {
  auto conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return true;
  }
  auto stmt = conn->prepare("select name from projects order by id");
  if (!stmt) {
    return true;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != planar::db::step_result::row) {
    return true;
  }
  return stmt->is_null(0);
}

/// @brief Read every `project_associations` row as `<project>|<assoc>|<source>`.
/// @param fx The fixture.
/// @return The rows, ordered by project id.
auto read_memberships(const fixture& fx) -> std::vector<std::string> {
  std::vector<std::string> rows;
  auto                     conn = planar::db::connection::open(fx.db_path.string());
  if (!conn) {
    return rows;
  }
  auto stmt = conn->prepare("select project_id, association_id, source from project_associations order by project_id");
  if (!stmt) {
    return rows;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped != planar::db::step_result::row) {
      return rows;
    }
    rows.push_back(std::format("{}|{}|{}", stmt->column_int64(0), stmt->column_int64(1), stmt->column_text(2)));
  }
}

/// @brief `init` the fixture's project and join it to a fresh association,
/// entirely through the CLI.
///
/// Unlike the task-6133 cases just above, which had to reach into the
/// engine directly because `assoc add` did not exist yet, this goes through
/// the real verb — so the setup itself is coverage.
/// @param fx The fixture.
/// @param assoc_slug The association to create and join.
auto associate_cwd(const fixture& fx, std::string_view assoc_slug) -> void {
  REQUIRE(dispatch(fx, {"init"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", std::string{assoc_slug}, "--kind", "project"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", std::string{assoc_slug}, (fx.root / "proj").string()}).code == 0);
}

} // namespace

TEST_CASE("task add threads EVERY optional flag into the row", "[cmd][handlers][task][parity][6135]") {
  // THE anti-defaulting case, and the reason this verb is not plumbing.
  // `task_create_args` default-constructs all eight optional members, so a
  // handler that drops any single one still compiles, still exits 0, and
  // still prints a row that reads correctly at a glance. Each assertion
  // below fails on exactly one dropped argument, and each reads the COLUMN
  // rather than the rendered line.
  auto const fx = make_fixture("taflags");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"plan", "create", "P one"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "anchor"}).code == 0);

  auto const got = dispatch(fx, {"task", "add", "full task", "--body", "B", "--next-action", "NA", "--due", "2026-09-01",
                                 "--plan", "1", "--parent", "1", "--slug", "full-task", "--priority", "3"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  // Thirteen-column label padding, wider than `plan create`'s nine and
  // `assoc create`'s ten. `plan`, `parent`, `next action`, `due` and
  // `body` are the five conditional lines, in exactly this order.
  CHECK(got.out.starts_with("id:          2\n"
                            "title:       full task\n"
                            "status:      todo\n"
                            "priority:    3\n"
                            "scope:       association:1\n"
                            "plan:        1\n"
                            "parent:      1\n"
                            "next action: NA\n"
                            "due:         2026-09-01\n"
                            "body:        B\n"
                            "created:     "));
  CHECK(got.out.contains("\nupdated:     "));
  CHECK(got.out.ends_with("\n"));

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[1].title == "full task");
  CHECK(rows[1].body == "B");            // --body
  CHECK(rows[1].slug == "full-task");    // --slug
  CHECK(rows[1].plan_id == "1");         // --plan
  CHECK(rows[1].parent_task_id == "1");  // --parent
  CHECK(rows[1].next_action == "NA");    // --next-action
  CHECK(rows[1].due_at == "2026-09-01"); // --due
  CHECK(rows[1].priority == 3);          // --priority
  CHECK(rows[1].status == "todo");
  // And the scope the CWD derived, which no flag supplied.
  CHECK(rows[1].scope_kind == "association");
  CHECK(rows[1].scope_id == "1");
}

TEST_CASE("task add writes NULL, not empty string, for every unsupplied optional", "[cmd][handlers][task][parity][6135]") {
  // The other half of the same invariant, and the half an exit-code test
  // can never see. A handler that passed `std::string{}` instead of
  // `std::nullopt` produces rows that render IDENTICALLY — the text
  // renderer's conditional lines are keyed on has_value(), so an empty
  // string would print `body:        ` where NULL prints nothing, but the
  // JSON `""` vs `null` and the column `''` vs NULL are the real tells.
  auto const fx = make_fixture("tanull");
  associate_cwd(fx, "acme");

  auto const got = dispatch(fx, {"task", "add", "minimal json", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with(R"({"id":1,"scope_kind":"association","scope_id":1,"plan_id":null,)"
                            R"("parent_task_id":null,"title":"minimal json","body":null,"slug":null,)"
                            R"("status":"todo","priority":100,"next_action":null,"due_at":null,)"));
  CHECK(got.out.ends_with("}\n"));

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].body == "~");
  CHECK(rows[0].slug == "~");
  CHECK(rows[0].plan_id == "~");
  CHECK(rows[0].parent_task_id == "~");
  CHECK(rows[0].next_action == "~");
  CHECK(rows[0].due_at == "~");
  CHECK(rows[0].priority == 100); // the DECLARED default, not 0
}

TEST_CASE("task add --due '' stores an empty string, distinct from NULL", "[cmd][handlers][task][parity][6135]") {
  // The case that proves the NULL sentinel above is not vacuous. zig's
  // `parseDueAt` returns the empty string unchanged rather than rejecting
  // it, so `--due ''` is accepted and lands as `''` — a value the previous
  // case's `"~"` would have flagged.
  auto const fx = make_fixture("taempty");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"task", "add", "due empty", "--due", ""}).code == 0);

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].due_at.empty()); // '' — NOT the "~" NULL sentinel
  CHECK(rows[0].due_at != "~");
}

TEST_CASE("task add does NOT refuse an unassociated project", "[cmd][handlers][task][parity][6135]") {
  // The asymmetry with `plan create`, which refuses at exit 5 in exactly
  // this fixture. Copying that refusal across because the two verbs look
  // alike would break every `task add` run from an unassociated repo.
  // Oracle-verified: exit 0, `scope: global`, a real row.
  auto const fx = make_fixture("taunassoc");
  REQUIRE(dispatch(fx, {"init"}).code == 0);

  auto const got = dispatch(fx, {"task", "add", "outside any association"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out.contains("scope:       global\n"));

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].scope_kind == "global");
  CHECK(rows[0].scope_id == "~");
}

TEST_CASE("task add --scope wins over the cwd and reaches the row", "[cmd][handlers][task][parity][6135]") {
  auto const fx = make_fixture("tascope");
  associate_cwd(fx, "acme");

  REQUIRE(dispatch(fx, {"task", "add", "Repo Scoped", "--scope", "repo:proj"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Global Explicit", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Cwd Derived"}).code == 0);

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].scope_kind == "repo");
  CHECK(rows[0].scope_id == "1");
  CHECK(rows[1].scope_kind == "global");
  CHECK(rows[1].scope_id == "~");
  CHECK(rows[2].scope_kind == "association");
  CHECK(rows[2].scope_id == "1");
}

TEST_CASE("task add renders a negative priority as 0 in text and unclamped in JSON", "[cmd][handlers][task][parity][6135]") {
  // Oracle-captured on both wires from the same `--priority -5` run:
  // `renderText` prints `@max(priority, 0)` while `std.json.Stringify`
  // serializes the field. The COLUMN keeps -5 either way, so "the renderer
  // clamps" is not "the value is clamped".
  auto const fx = make_fixture("taneg");
  associate_cwd(fx, "acme");

  auto const text = dispatch(fx, {"task", "add", "neg pri", "--priority", "-5"});
  REQUIRE(text.code == 0);
  CHECK(text.out.contains("priority:    0\n"));
  CHECK_FALSE(text.out.contains("-5"));

  auto const json = dispatch(fx, {"task", "add", "neg pri json", "--priority", "-5", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.contains(R"("priority":-5,)"));

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].priority == -5);
  CHECK(rows[1].priority == -5);
}

TEST_CASE("task add refuses a malformed --due at exit 1 without writing", "[cmd][handlers][task][parity][6135]") {
  // `due_at` validation was recorded as NOT ported when the engine landed,
  // on the grounds that it was a cmd-layer concern. The oracle validates
  // inside `task.zig`'s `create`, so the check lives in the engine here
  // too — see `task_error::invalid_due_at`. Without it these three all
  // exit 0 and store garbage.
  auto const fx = make_fixture("tadue");
  associate_cwd(fx, "acme");

  for (auto const& bad : {"not-a-date", "2026-02-30", "2026-09-01T10:00:00Z ", "2026-13-01", "2026-09-01T25:00:00Z"}) {
    INFO("--due " << bad);
    auto const got = dispatch(fx, {"task", "add", "bad due", "--due", bad});
    CHECK(got.code == 1);
    CHECK(got.err == "error: task add: InvalidDueAt\n");
    CHECK(got.out.empty());
  }
  CHECK(read_tasks(fx).empty());

  // The accepted shapes, for contrast: bare date, `Z`, fractional +offset.
  for (auto const& good : {"2026-09-01", "2026-09-01T10:00:00Z", "2026-09-01T10:00:00.123+05:30", "2024-02-29"}) {
    INFO("--due " << good);
    CHECK(dispatch(fx, {"task", "add", "good due", "--due", good}).code == 0);
  }
  CHECK(read_tasks(fx).size() == 4);
}

TEST_CASE("task add resolves --scope BEFORE validating --due", "[cmd][handlers][task][parity][6135]") {
  // Both failures exit 1, so only the MESSAGE separates them — and the
  // order is the oracle's (`resolveSlug` at task.zig:309, `parseDueAt` at
  // :323). Validating the due date in the handler instead of the engine
  // would silently invert this.
  auto const fx = make_fixture("taorder");
  associate_cwd(fx, "acme");

  auto const got = dispatch(fx, {"task", "add", "both bad", "--scope", "nosuchscope", "--due", "garbage"});
  CHECK(got.code == 1);
  CHECK(got.err == "error: task add: SlugNotFound\n");
  CHECK(read_tasks(fx).empty());
}

TEST_CASE("task add maps a slug collision to exit 6 and a dangling --plan to exit 1", "[cmd][handlers][task][parity][6135]") {
  auto const fx = make_fixture("tafail");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"task", "add", "first", "--slug", "taken"}).code == 0);

  // `tasks.slug` is GLOBALLY unique — the collision does not have to be
  // within a plan, which is a documented Planar behaviour and a real
  // operator failure mode.
  auto const dup = dispatch(fx, {"task", "add", "second", "--slug", "taken"});
  CHECK(dup.code == 6);
  CHECK(dup.err == "error: task add: SlugConflict\n");

  auto const bad_plan = dispatch(fx, {"task", "add", "orphan", "--plan", "999"});
  CHECK(bad_plan.code == 1);
  CHECK(bad_plan.err == "error: task add: QueryFailed\n");

  auto const bad_parent = dispatch(fx, {"task", "add", "orphan", "--parent", "999"});
  CHECK(bad_parent.code == 1);
  CHECK(bad_parent.err == "error: task add: QueryFailed\n");

  auto const bad_scope = dispatch(fx, {"task", "add", "unscoped", "--scope", "nosuchscope"});
  CHECK(bad_scope.code == 1);
  CHECK(bad_scope.err == "error: task add: SlugNotFound\n");

  // Exactly one row survives all four refusals.
  CHECK(read_tasks(fx).size() == 1);
}

TEST_CASE("task add parses --priority with Zig's integer separators", "[cmd][handlers][task][parity][6135]") {
  // `1_0` is TEN. The `zig_int_validator` on the declared flag is what
  // keeps this from degrading into a silent "absent" and the 100 default.
  auto const fx = make_fixture("tapri");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"task", "add", "underscore", "--priority", "1_0"}).code == 0);

  auto const rows = read_tasks(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].priority == 10);
}

TEST_CASE("assoc add registers the project, joins it, and reports both argv values", "[cmd][handlers][assoc][parity][6135]") {
  auto const fx = make_fixture("aaok");
  REQUIRE(dispatch(fx, {"init"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "acme", "--kind", "project"}).code == 0);

  auto const path = (fx.root / "proj").string();
  auto const got  = dispatch(fx, {"assoc", "add", "acme", path});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  // Built from the ARGUMENTS, not read back from the row — `add_member`
  // returns void, there is nothing to render.
  CHECK(got.out == std::format("added project at {} to acme\n", path));

  auto const memberships = read_memberships(fx);
  REQUIRE(memberships.size() == 1);
  CHECK(memberships[0] == "1|1|user"); // source='user', not an auto-detect value
}

TEST_CASE("assoc add auto-registers an unregistered path, verbatim", "[cmd][handlers][assoc][parity][6135]") {
  // The path is the `projects.root_path` KEY cwd-derive later matches
  // against, so it is stored exactly as typed: not canonicalised, not made
  // absolute, and not required to exist. Each of those would look like a
  // hardening and would break the match.
  auto const fx = make_fixture("aapath");
  REQUIRE(dispatch(fx, {"assoc", "create", "acme"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", "/nonexistent/path/xyz"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", "relative/path"}).code == 0);

  auto const rows = read_projects(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].slug == "xyz");
  CHECK(rows[0].name == "xyz");
  CHECK(rows[0].root_path == "/nonexistent/path/xyz"); // verbatim
  CHECK(rows[1].slug == "path");
  CHECK(rows[1].root_path == "relative/path"); // still verbatim, still relative
}

TEST_CASE("assoc add derives the basename the way zig does, trailing slash included", "[cmd][handlers][assoc][parity][6135]") {
  // A REAL divergence this cycle found and closed. The engine used
  // `std::filesystem::path::filename`, which returns EMPTY for a path
  // ending in a separator, where zig's `std.fs.path.basename` strips the
  // separator first. Both binaries exited 0 with identical stdout; only
  // the row differed — `slug='_', name=''` here against `slug='repo-2',
  // name='repo'` on the oracle. The `_` is `slugify_path_segment`'s
  // empty-input fallback, which is the tell.
  auto const fx = make_fixture("aaslash");
  REQUIRE(dispatch(fx, {"assoc", "create", "acme"}).code == 0);
  auto const path = (fx.root / "proj").string();
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path + "/"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path + "//"}).code == 0);

  auto const rows = read_projects(fx);
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].slug == "proj");
  // Distinct root_path values, so these are distinct projects — each one
  // takes the next free numeric suffix on the colliding derived slug.
  CHECK(rows[1].slug == "proj-2");
  CHECK(rows[1].name == "proj"); // NOT ""
  CHECK(rows[2].slug == "proj-3");
  CHECK(rows[2].name == "proj");
}

TEST_CASE("assoc add accepts the root path, whose basename is empty", "[cmd][handlers][assoc][parity][6135]") {
  // The empty basename is bound to a NOT NULL column. A `string_view`
  // returned by value from the basename helper would be NULL-data here,
  // `sqlite3_bind_text(nullptr, 0)` binds SQL NULL, and the verb would die
  // `QueryFailed` where the oracle exits 0. Regression pin.
  auto const fx = make_fixture("aaroot");
  REQUIRE(dispatch(fx, {"assoc", "create", "acme"}).code == 0);
  auto const got = dispatch(fx, {"assoc", "add", "acme", "/"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());

  auto const rows = read_projects(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].slug == "_"); // the slugify empty-input fallback
  CHECK(rows[0].name.empty());
  CHECK_FALSE(project_name_is_null(fx)); // '' in the column, NOT NULL
  CHECK(rows[0].root_path == "/");
}

TEST_CASE("assoc add refuses an unknown association and a duplicate membership at exit 1",
          "[cmd][handlers][assoc][parity][6135]") {
  auto const fx = make_fixture("aafail");
  REQUIRE(dispatch(fx, {"assoc", "create", "acme"}).code == 0);
  auto const path = (fx.root / "proj").string();

  auto const unknown = dispatch(fx, {"assoc", "add", "nosuch", path});
  CHECK(unknown.code == 1);
  CHECK(unknown.err == "error: no association named 'nosuch'\n");
  CHECK(unknown.out.empty());
  CHECK(read_projects(fx).empty()); // the association lookup precedes the project write

  REQUIRE(dispatch(fx, {"assoc", "add", "acme", path}).code == 0);
  auto const again = dispatch(fx, {"assoc", "add", "acme", path});
  // ONE, not six. `AlreadyMember` is a duplicate collision but is NOT in
  // `codeFor`'s slug-conflict arm, which matches only `error.SlugConflict`
  // and `error.AlreadyExists`.
  CHECK(again.code == 1);
  CHECK(again.err == std::format("error: project at '{}' is already a member of 'acme'\n", path));
  CHECK(read_memberships(fx).size() == 1);
}

TEST_CASE("assoc add --json emits the four-field literal, unescaped", "[cmd][handlers][assoc][parity][6135]") {
  auto const fx = make_fixture("aajson");
  REQUIRE(dispatch(fx, {"assoc", "create", "acme"}).code == 0);
  auto const path = (fx.root / "proj").string();

  auto const got = dispatch(fx, {"assoc", "add", "acme", path, "--json"});
  REQUIRE(got.code == 0);
  // `"status":"added"` is a field that exists nowhere in the schema.
  CHECK(got.out == std::format("{{\"status\":\"added\",\"association\":\"acme\",\"repo_path\":\"{}\"}}\n", path));

  // And the oracle's unescaped interpolation, pinned rather than fixed: a
  // quote in either value produces INVALID JSON on both binaries. Captured
  // from `assoc add 'q"uote' '/tmp/pa"th' --json`.
  REQUIRE(dispatch(fx, {"assoc", "create", "q\"uote"}).code == 0);
  auto const raw = dispatch(fx, {"assoc", "add", "q\"uote", "/tmp/pa\"th", "--json"});
  REQUIRE(raw.code == 0);
  CHECK(raw.out == "{\"status\":\"added\",\"association\":\"q\"uote\",\"repo_path\":\"/tmp/pa\"th\"}\n");
}

// =========================================================================
// task 6141 — the `plan` and `task` read/update leaves
//
// Every case below reads the resulting DATABASE ROWS, not only the rendered
// line. That is not belt-and-braces: the four silent-degradation defects
// this port has already closed (tasks 6128, 6132, 6133, 6135) each exited 0
// with oracle-identical stdout and a wrong row, so a stdout assertion alone
// would have passed on all four. The read verbs have their own version of
// the same shape — a filter that is silently ignored still returns
// plausible rows — so every filter flag here is asserted to actually
// EXCLUDE something.
// =========================================================================

namespace {

/// @brief Seed a fixture with an association, two plans and four tasks
/// spread across scopes and priorities, so a filter has something to
/// exclude.
/// @param fx The fixture.
auto seed_planning(const fixture& fx) -> void {
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"plan", "create", "First plan", "--summary", "S1"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Child plan", "--parent", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T low pri", "--plan", "1", "--priority", "10"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T high pri", "--plan", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T other plan", "--plan", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T global", "--scope", "global"}).code == 0);
}

} // namespace

TEST_CASE("plan show renders the key/value block and refuses a non-integer id at exit 2", "[cmd][handlers][plan][parity][6141]") {
  auto const fx = make_fixture("plshow");
  seed_planning(fx);

  auto const got = dispatch(fx, {"plan", "show", "1"});
  REQUIRE(got.code == 0);
  CHECK(got.err.empty());
  // Nine-column labels, and `summary` AFTER the (absent) `parent` line.
  CHECK(got.out.starts_with("id:       1\n"
                            "title:    First plan\n"
                            "slug:     first-plan\n"
                            "status:   draft\n"
                            "scope:    association:1\n"
                            "summary:  S1\n"
                            "created:  "));
  CHECK(got.out.ends_with("\n"));

  // The id positional is a STRING the handler parses itself, so a bad value
  // is exit 2 with the handler's own wording — NOT the parser's exit 1.
  auto const bad = dispatch(fx, {"plan", "show", "abc"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: plan id must be an integer, got 'abc'\n");

  auto const missing = dispatch(fx, {"plan", "show", "99"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no plan with id 99\n");
}

TEST_CASE("plan show --json emits null, not empty string, for every unset optional", "[cmd][handlers][plan][parity][6141]") {
  auto const fx = make_fixture("plshowj");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"plan", "create", "Bare"}).code == 0);

  auto const got = dispatch(fx, {"plan", "show", "1", "--json"});
  REQUIRE(got.code == 0);
  CHECK(got.out == R"({"id":1,"scope_kind":"association","scope_id":1,"title":"Bare","slug":"bare",)"
                   R"("summary":null,"status":"draft","parent_plan_id":null,)" +
                       got.out.substr(got.out.find(R"("created_at")")));
  CHECK(got.out.contains(R"("summary":null)"));
  CHECK(!got.out.contains(R"("summary":"")"));
  CHECK(got.out.ends_with("}\n"));

  auto const rows = read_plans(fx);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].summary == "~"); // NULL in the column, not ''
  CHECK(rows[0].parent_plan_id == "~");
}

TEST_CASE("plan list filters actually filter, and an empty list is '(no plans)'", "[cmd][handlers][plan][parity][6141]") {
  auto const fx = make_fixture("pllist");
  seed_planning(fx);

  // Unfiltered: both plans, id-ordered, four columns.
  auto const all = dispatch(fx, {"plan", "list"});
  REQUIRE(all.code == 0);
  CHECK(all.out == "    1  draft       first-plan                First plan\n"
                   "    2  draft       child-plan                Child plan\n");

  // --parent EXCLUDES. A filter that was accepted and ignored would return
  // both rows here and read as success.
  auto const child = dispatch(fx, {"plan", "list", "--parent", "1"});
  REQUIRE(child.code == 0);
  CHECK(child.out == "    2  draft       child-plan                Child plan\n");

  // A filter matching nothing renders the literal sentinel, not "".
  auto const none = dispatch(fx, {"plan", "list", "--parent", "99"});
  REQUIRE(none.code == 0);
  CHECK(none.out == "(no plans)\n");

  // --status is COMMA-SEPARATED on this verb, and tolerates spaces.
  CHECK(dispatch(fx, {"plan", "list", "--status", "draft"}).out == all.out);
  CHECK(dispatch(fx, {"plan", "list", "--status", " draft , active "}).out == all.out);
  CHECK(dispatch(fx, {"plan", "list", "--status", "active"}).out == "(no plans)\n");

  auto const bogus = dispatch(fx, {"plan", "list", "--status", "bogus"});
  CHECK(bogus.code == 1); // exit 1, NOT the parser's 2
  CHECK(bogus.err == "error: unknown status 'bogus'\n");

  // --scope EXCLUDES too: no plan lives in global.
  CHECK(dispatch(fx, {"plan", "list", "--scope", "global"}).out == "(no plans)\n");
  auto const nosuch = dispatch(fx, {"plan", "list", "--scope", "nosuch"});
  CHECK(nosuch.code == 1);
  CHECK(nosuch.err == "error: plan list: SlugNotFound\n");

  // --json is an ARRAY on one line, and empty renders `[]`.
  auto const json = dispatch(fx, {"plan", "list", "--json"});
  REQUIRE(json.code == 0);
  CHECK(json.out.starts_with(R"([{"id":1,)"));
  CHECK(json.out.ends_with("}]\n"));
  CHECK(dispatch(fx, {"plan", "list", "--parent", "99", "--json"}).out == "[]\n");
}

TEST_CASE("plan list hides terminal plans unless a status names them", "[cmd][handlers][plan][parity][6141]") {
  // The engine defect this cycle exposed. `plan_list_filter{}` with no
  // statuses is NOT "every status" — it is the three OPEN ones. The C++
  // engine applied no predicate at all, which was invisible while no verb
  // called it, and would have listed closed plans the oracle hides.
  auto const fx = make_fixture("plterm");
  seed_planning(fx);
  REQUIRE(dispatch(fx, {"plan", "update", "2", "--status", "active"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "update", "2", "--status", "done"}).code == 0);

  auto const rows = read_plans(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[1].status == "done"); // the row IS terminal…

  auto const open = dispatch(fx, {"plan", "list"});
  REQUIRE(open.code == 0);
  CHECK(!open.out.contains("child-plan")); // …and the default listing hides it
  CHECK(open.out.contains("first-plan"));

  // Naming it explicitly brings it back, which is what proves the default
  // is a real predicate rather than an accident of ordering.
  auto const closed = dispatch(fx, {"plan", "list", "--status", "done"});
  REQUIRE(closed.code == 0);
  CHECK(closed.out == "    2  done        child-plan                Child plan\n");
}

TEST_CASE("plan list refuses when the cwd pins no scope", "[cmd][handlers][plan][parity][6141]") {
  // An empty read set must REFUSE. Falling back to an unfiltered listing
  // would be the read-verb form of `plan create`'s silent-`global` defect:
  // exit 0, plausible rows, every scope in the database.
  auto const fx = make_fixture("plnoscope");
  REQUIRE(dispatch(fx, {"init"}).code == 0);
  // `init` registers the cwd as a PROJECT but joins it to no association,
  // so cwd derives a repo scope... which is a scope. Point the fixture at a
  // directory that is registered nowhere instead.
  auto outside        = fx;
  outside.vars["PWD"] = (fx.root / "elsewhere").string();
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "elsewhere", ec);

  std::ostringstream out;
  std::ostringstream err;
  context    ctx{{"planar", "plan", "list"}, planar::cmd::map_env(outside.vars), fx.root / "elsewhere", fx.db_path, out, err};
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::handlers(*tree);
  CHECK(planar::cmd::run(ctx, *tree, table) == 1);
  CHECK(err.str() == "error: cwd is not inside any registered Planar scope; cd into a registered scope or pass "
                     "--scope global\n");
  CHECK(out.str().empty());
}

TEST_CASE("plan list --touches refuses an unknown repo rather than ignoring the filter",
          "[cmd][handlers][plan][parity][6141][6187]") {
  // REPLACES the exit-64 assertion this case carried from task 6141, when
  // `listTouching` was unported and the flag refused wholesale. Task 6187
  // landed `list_plans_touching` / `list_tasks_touching`, so the flag now
  // WORKS — but the property the original case existed to protect is
  // unchanged and is what is asserted here instead: an unresolvable repo
  // slug must REFUSE, never fall through to an unfiltered or empty list,
  // because either would be indistinguishable from success.
  //
  // The positive filtering behaviour — including the branch asymmetry that
  // makes the scope predicate apply to only one half of the UNION — is
  // covered in `plan_task_remainder_leaves.t.cpp`, on a fixture with rows
  // on both sides of every predicate.
  auto const fx = make_fixture("pltouch");
  seed_planning(fx);
  auto const got = dispatch(fx, {"plan", "list", "--touches", "acme"});
  CHECK(got.code == 1);
  CHECK(got.err == "error: repo 'acme' not found\n");
  CHECK(got.out.empty());

  auto const tgot = dispatch(fx, {"task", "list", "--touches", "acme"});
  CHECK(tgot.code == 1);
  CHECK(tgot.err == "error: repo 'acme' not found\n");
  CHECK(tgot.out.empty());
}

TEST_CASE("plan update writes every supplied field and treats --parent 0 as CLEAR", "[cmd][handlers][plan][parity][6141]") {
  auto const fx = make_fixture("plupd");
  seed_planning(fx);

  auto const got = dispatch(fx, {"plan", "update", "2", "--title", "Renamed", "--slug", "renamed", "--summary", "S2"});
  REQUIRE(got.code == 0);
  auto rows = read_plans(fx);
  REQUIRE(rows.size() == 2);
  CHECK(rows[1].title == "Renamed");
  CHECK(rows[1].slug == "renamed");
  CHECK(rows[1].summary == "S2");
  CHECK(rows[1].parent_plan_id == "1"); // untouched by this patch

  // `--parent 0` CLEARS. Threaded through as an id it would write a
  // dangling key (or fail) instead.
  REQUIRE(dispatch(fx, {"plan", "update", "2", "--parent", "0"}).code == 0);
  rows = read_plans(fx);
  CHECK(rows[1].parent_plan_id == "~"); // NULL, not 0
  CHECK(rows[1].title == "Renamed");    // and nothing else moved

  // A non-zero value reassigns.
  REQUIRE(dispatch(fx, {"plan", "update", "2", "--parent", "1"}).code == 0);
  CHECK(read_plans(fx)[1].parent_plan_id == "1");

  auto const bogus = dispatch(fx, {"plan", "update", "1", "--status", "bogus"});
  CHECK(bogus.code == 1);
  CHECK(bogus.err == "error: unknown status 'bogus'\n");
  CHECK(read_plans(fx)[0].status == "draft"); // refused means UNCHANGED

  auto const missing = dispatch(fx, {"plan", "update", "99", "--title", "X"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no plan with id 99\n");
}

TEST_CASE("plan update warns on open descendants when closing, and still writes", "[cmd][handlers][plan][parity][6141]") {
  // An ADVISORY, not a refusal — and suppressed under --json.
  auto const fx = make_fixture("plwarn");
  seed_planning(fx);
  REQUIRE(dispatch(fx, {"plan", "update", "1", "--status", "active"}).code == 0);

  auto const got = dispatch(fx, {"plan", "update", "1", "--status", "done"});
  CHECK(got.code == 0); // NOT refused
  CHECK(got.err.contains("warning: plan 1 still has 1 open descendant plan(s)"));
  CHECK(got.err.contains("planar plan closeout 1"));
  CHECK(read_plans(fx)[0].status == "done"); // and the write landed

  // With no open descendants, no advisory at all.
  REQUIRE(dispatch(fx, {"plan", "update", "2", "--status", "active"}).code == 0);
  auto const quiet = dispatch(fx, {"plan", "update", "2", "--status", "done"});
  CHECK(quiet.code == 0);
  CHECK(quiet.err.empty());
}

TEST_CASE("plan recompute-status requires exactly one of --plan / --all", "[cmd][handlers][plan][parity][6141]") {
  auto const fx = make_fixture("plrecomp");
  seed_planning(fx);

  auto const neither = dispatch(fx, {"plan", "recompute-status"});
  CHECK(neither.code == 2);
  CHECK(neither.err == "error: either --plan <id> or --all is required\n");

  auto const both = dispatch(fx, {"plan", "recompute-status", "--plan", "1", "--all"});
  CHECK(both.code == 2);
  CHECK(both.err == "error: --plan and --all are mutually exclusive\n");

  auto const missing = dispatch(fx, {"plan", "recompute-status", "--plan", "99"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no plan with id 99\n");
}

TEST_CASE("plan recompute-status flips a plan and reports the transition", "[cmd][handlers][plan][parity][6141]") {
  auto const fx = make_fixture("plrecomp2");
  seed_planning(fx);
  // Move a task to `doing` WITHOUT auto-promotion, so the plan is stale and
  // the recompute has something to do. Without --no-auto-promote the update
  // would already have promoted it and this case would assert nothing.
  REQUIRE(dispatch(fx, {"task", "update", "1", "--status", "doing", "--no-auto-promote"}).code == 0);
  REQUIRE(read_plans(fx)[0].status == "draft");

  auto const one = dispatch(fx, {"plan", "recompute-status", "--plan", "1"});
  REQUIRE(one.code == 0);
  CHECK(one.out == "plan 1: draft \xe2\x86\x92 active\n"); // U+2192, not "->"
  CHECK(read_plans(fx)[0].status == "active");

  // Idempotent: a second run reports no change and writes nothing.
  auto const again = dispatch(fx, {"plan", "recompute-status", "--plan", "1"});
  CHECK(again.out == "plan 1: active (no change)\n");

  auto const json = dispatch(fx, {"plan", "recompute-status", "--plan", "1", "--json"});
  CHECK(json.out == R"({"plan_id":1,"status_before":"active","status_after":"active","flipped":false})"
                    "\n");

  // `--all` prints only TRANSITIONS, then a tally led by a blank line.
  auto const all = dispatch(fx, {"plan", "recompute-status", "--all"});
  REQUIRE(all.code == 0);
  CHECK(all.out == "\nrecomputed 2 plans; 0 transitioned\n");
}

TEST_CASE("task show renders the block and refuses a non-integer id at exit 2", "[cmd][handlers][task][parity][6141]") {
  auto const fx = make_fixture("tkshow");
  seed_planning(fx);

  auto const got = dispatch(fx, {"task", "show", "1"});
  REQUIRE(got.code == 0);
  CHECK(got.out.starts_with("id:          1\n"
                            "title:       T low pri\n"
                            "status:      todo\n"
                            "priority:    10\n"
                            "scope:       association:1\n"
                            "plan:        1\n"
                            "created:     "));

  auto const bad = dispatch(fx, {"task", "show", "abc"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: task id must be an integer, got 'abc'\n");

  auto const missing = dispatch(fx, {"task", "show", "99"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no task with id 99\n");
}

TEST_CASE("task show clamps a negative priority in text but not in JSON or the column", "[cmd][handlers][task][parity][6141]") {
  auto const fx = make_fixture("tkneg");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"task", "add", "neg", "--priority", "-5"}).code == 0);

  CHECK(dispatch(fx, {"task", "show", "1"}).out.contains("priority:    0\n"));
  CHECK(dispatch(fx, {"task", "show", "1", "--json"}).out.contains(R"("priority":-5,)"));
  CHECK(read_tasks(fx)[0].priority == -5);
  // And the list renderer clamps too, in its own three-wide column.
  CHECK(dispatch(fx, {"task", "list"}).out == "    1  pri   0  todo        neg\n");
}

TEST_CASE("task list filters actually filter, and default hides terminal tasks", "[cmd][handlers][task][parity][6141]") {
  auto const fx = make_fixture("tklist");
  seed_planning(fx);

  // Default: the association's three OPEN tasks, priority-ordered. The
  // global task is excluded by the cwd read set, not by luck.
  auto const all = dispatch(fx, {"task", "list"});
  REQUIRE(all.code == 0);
  CHECK(all.out == "    1  pri  10  todo        T low pri\n"
                   "    3  pri 100  todo        T other plan\n"
                   "    2  pri 100  todo        T high pri\n");
  CHECK(!all.out.contains("T global"));

  // Each filter must EXCLUDE something.
  CHECK(dispatch(fx, {"task", "list", "--plan", "2"}).out == "    3  pri 100  todo        T other plan\n");
  CHECK(dispatch(fx, {"task", "list", "--priority-max", "10"}).out == "    1  pri  10  todo        T low pri\n");
  CHECK(dispatch(fx, {"task", "list", "--scope", "global"}).out == "    4  pri 100  todo        T global\n");
  CHECK(dispatch(fx, {"task", "list", "--status", "done"}).out == "(no tasks)\n");

  // `--status` here is ONE value, unlike `plan list`'s comma list. The
  // asymmetry is the oracle's; a helpful "fix" would diverge.
  auto const list_status = dispatch(fx, {"task", "list", "--status", "todo,doing"});
  CHECK(list_status.code == 1);
  CHECK(list_status.err == "error: unknown status 'todo,doing'\n");

  // …and `--scope` here is NOT comma-split either.
  auto const list_scope = dispatch(fx, {"task", "list", "--scope", "global,acme"});
  CHECK(list_scope.code == 1);
  CHECK(list_scope.err == "error: task list: SlugNotFound\n");

  // A done task disappears from the default listing but is still a row.
  // `todo -> done` is an ILLEGAL transition (the matrix routes through
  // `doing`), so the intermediate step is required rather than tidy-up.
  REQUIRE(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "1"}).code == 0);
  CHECK(!dispatch(fx, {"task", "list"}).out.contains("T low pri"));
  CHECK(dispatch(fx, {"task", "list", "--status", "done"}).out.contains("T low pri"));
  CHECK(read_tasks(fx)[0].status == "done");

  CHECK(dispatch(fx, {"task", "list", "--status", "done", "--json"}).out.starts_with(R"([{"id":1,)"));
  CHECK(dispatch(fx, {"task", "list", "--status", "cancelled", "--json"}).out == "[]\n");
}

TEST_CASE("task update writes every supplied field and treats --plan 0 as CLEAR", "[cmd][handlers][task][parity][6141]") {
  auto const fx = make_fixture("tkupd");
  seed_planning(fx);

  REQUIRE(dispatch(fx, {"task", "update", "1", "--title", "Renamed", "--body", "B2", "--next-action", "NA2", "--due",
                        "2026-12-31", "--slug", "renamed", "--priority", "4"})
              .code == 0);
  auto rows = read_tasks(fx);
  CHECK(rows[0].title == "Renamed");
  CHECK(rows[0].body == "B2");
  CHECK(rows[0].next_action == "NA2");
  CHECK(rows[0].due_at == "2026-12-31");
  CHECK(rows[0].slug == "renamed");
  CHECK(rows[0].priority == 4);
  CHECK(rows[0].plan_id == "1"); // untouched

  REQUIRE(dispatch(fx, {"task", "update", "1", "--plan", "0"}).code == 0);
  rows = read_tasks(fx);
  CHECK(rows[0].plan_id == "~"); // NULL, not 0
  CHECK(rows[0].title == "Renamed");

  REQUIRE(dispatch(fx, {"task", "update", "1", "--plan", "2"}).code == 0);
  CHECK(read_tasks(fx)[0].plan_id == "2");

  auto const bogus = dispatch(fx, {"task", "update", "1", "--status", "bogus"});
  CHECK(bogus.code == 1);
  CHECK(read_tasks(fx)[0].status == "todo");

  auto const missing = dispatch(fx, {"task", "update", "99", "--title", "X"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no task with id 99\n");
}

TEST_CASE("task update refuses a cross-scope write at exit 5", "[cmd][handlers][task][parity][6141]") {
  auto const fx = make_fixture("tkscope");
  seed_planning(fx);

  // Task 1 lives on the association; `--scope global` is a different scope.
  auto const got = dispatch(fx, {"task", "update", "1", "--scope", "global", "--title", "X"});
  CHECK(got.code == 5);
  CHECK(got.err == "error: scope mismatch: task 1 is in scope 'acme' but operator write scope is 'global'; pass "
                   "--scope acme to write to that scope from here\n");
  CHECK(read_tasks(fx)[0].title == "T low pri"); // refused means UNCHANGED

  // A GLOBAL task has no scope label, so the guard allows any write —
  // asserted so the refusal above is not mistaken for "any --scope fails".
  CHECK(dispatch(fx, {"task", "update", "4", "--title", "GlobalRenamed"}).code == 0);
  CHECK(read_tasks(fx)[3].title == "GlobalRenamed");
}

TEST_CASE("task done / cancel / block / reopen write the row and the audit rows", "[cmd][handlers][task][parity][6141]") {
  auto const fx = make_fixture("tkflip");
  seed_planning(fx);

  // `todo -> done` is illegal; the matrix routes through `doing`.
  auto const straight_to_done = dispatch(fx, {"task", "done", "1"});
  CHECK(straight_to_done.code == 1);
  CHECK(straight_to_done.err == "error: task done: IllegalTransition\n");
  CHECK(read_tasks(fx)[0].status == "todo");

  REQUIRE(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "1"}).code == 0);
  CHECK(read_tasks(fx)[0].status == "done");

  REQUIRE(dispatch(fx, {"task", "cancel", "2"}).code == 0);
  CHECK(read_tasks(fx)[1].status == "cancelled");

  // `block` writes a status AND a `depends-on` edge; asserting only the
  // status would miss half the verb.
  REQUIRE(dispatch(fx, {"task", "block", "3", "--on", "4", "--reason", "waiting"}).code == 0);
  CHECK(read_tasks(fx)[2].status == "blocked");
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare("select from_kind, from_id, to_kind, to_id, relationship from entity_links");
    REQUIRE(stmt.has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_text(0) == "task");
    CHECK(stmt->column_int64(1) == 3);
    CHECK(stmt->column_text(2) == "task");
    CHECK(stmt->column_int64(3) == 4);
    CHECK(stmt->column_text(4) == "depends-on");
  }

  // `reopen` REQUIRES --reason, and the reason lands in `task_reopens`.
  auto const noreason = dispatch(fx, {"task", "reopen", "1"});
  CHECK(noreason.code == 2);
  CHECK(noreason.err == "error: --reason is required for reopen\n");
  CHECK(read_tasks(fx)[0].status == "done"); // refused means UNCHANGED

  REQUIRE(dispatch(fx, {"task", "reopen", "1", "--reason", "needs redo", "--status", "doing"}).code == 0);
  CHECK(read_tasks(fx)[0].status == "doing");
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare("select task_id, from_status, to_status, reason, source from task_reopens");
    REQUIRE(stmt.has_value());
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    REQUIRE(*stepped == planar::db::step_result::row);
    CHECK(stmt->column_int64(0) == 1);
    CHECK(stmt->column_text(1) == "done");
    CHECK(stmt->column_text(2) == "doing");
    CHECK(stmt->column_text(3) == "needs redo"); // NOT an empty audit row
    CHECK(stmt->column_text(4) == "task-reopen");
  }
}

TEST_CASE("an active work claim refuses every operator status flip until --force", "[cmd][handlers][task][parity][6141]") {
  // The guard the oracle runs inside its write transaction and this port
  // composes at layer 3, because engine_planning and engine_runtime are
  // both layer 2 and may not depend on each other. It became
  // operator-reachable for the first time when these verbs were wired: an
  // unguarded `task done` on a claimed task exits 0 and completes work
  // another agent is holding.
  auto const fx = make_fixture("tkclaim");
  seed_planning(fx);
  {
    // The claim row is inserted directly rather than through
    // `acquire_claim`, because that entry point needs a live `sessions` row
    // and a vendor tag this case does not otherwise care about. What the
    // guard actually reads is exactly these four columns plus an unexpired
    // lease, so this is the state under test, not a stand-in for it. The
    // `lease_expires_at` is an hour out so the case cannot pass by expiry.
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("insert into sessions (id, vendor) values (1, 'test')").has_value());
    REQUIRE(conn->execute("insert into agent_work_claims "
                          "  (claim_token, session_id, entity_kind, entity_id, status, vendor, lease_expires_at) "
                          "values ('tok6141', 1, 'task', 1, 'active', 'test', "
                          "        strftime('%Y-%m-%dT%H:%M:%fZ','now','+1 hour'))")
                .has_value());
  }

  auto const two_line = std::string{"error: task 1 has an active work claim — operator status flip refused.\n"
                                    "Release or complete the claim via the agent path, or re-run with --force to "
                                    "override.\n"};

  auto const done = dispatch(fx, {"task", "done", "1"});
  CHECK(done.code == 1);
  CHECK(done.err == two_line);
  CHECK(read_tasks(fx)[0].status != "done"); // and NOTHING was written

  CHECK(dispatch(fx, {"task", "block", "1", "--on", "2"}).err == two_line);
  CHECK(dispatch(fx, {"task", "reopen", "1", "--reason", "r"}).err == two_line);
  CHECK(dispatch(fx, {"task", "update", "1", "--status", "blocked"}).err == two_line);

  // `task cancel` has no --force and renders the GENERIC shape instead.
  auto const cancelled = dispatch(fx, {"task", "cancel", "1"});
  CHECK(cancelled.code == 1);
  CHECK(cancelled.err == "error: task cancel: TaskClaimed\n");

  // A NON-status patch is allowed on a claimed task — the guard is gated on
  // a status change. Gating it unconditionally would refuse a write the
  // oracle permits.
  REQUIRE(dispatch(fx, {"task", "update", "1", "--title", "StillAllowed"}).code == 0);
  CHECK(read_tasks(fx)[0].title == "StillAllowed");

  // …and --force overrides, which is what proves the refusals above are the
  // guard rather than some unrelated failure.
  REQUIRE(dispatch(fx, {"task", "done", "1", "--force"}).code == 0);
  CHECK(read_tasks(fx)[0].status == "done");
}

// ===========================================================================
// audit_log rows (plan 1001, task 6100)
// ===========================================================================

namespace {

/// @brief Every `audit_log` row, pipe-joined, one per line, with SQL NULL
/// rendered as the literal `<NULL>`.
///
/// Deliberately dumps the WHOLE table rather than counting or filtering.
/// A missing audit row is invisible in every other way -- no exit code, no
/// stdout byte, no rendered field moves -- so the assertion has to be the
/// full expected transcript. A count would pass against rows with the
/// wrong verb; a per-row lookup would pass against a table that also
/// contains rows the oracle does not write.
/// @param fx The fixture.
/// @return The transcript, newline-terminated per row.
auto audit_transcript(const fixture& fx) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select verb, entity_kind, entity_id, coalesce(actor, '<NULL>'), "
                            "coalesce(scope, '<NULL>'), coalesce(summary, '<NULL>') "
                            "from audit_log order by id");
  REQUIRE(stmt.has_value());
  std::string out;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    out += std::format("{}|{}|{}|{}|{}|{}\n", stmt->column_text(0), stmt->column_text(1), stmt->column_int64(2),
                       stmt->column_text(3), stmt->column_text(4), stmt->column_text(5));
  }
  return out;
}

} // namespace

// ORACLE PROVENANCE for both cases below: the exact same argv sequence was
// run against the Zig binary on an isolated scratch database
// (`PLANAR_DB` + `PLANAR_CONFIG_PATH`), `audit_log` was dumped with the
// same projection, and the two transcripts were diffed. They matched
// row-for-row and byte-for-byte. The only row the Zig run produced that
// this one cannot is `unlink|association|...` from `assoc remove`, a leaf
// that is not wired in this build at all (it exits 64) -- the engine's
// `remove_member` writes it and `association.t.cpp` pins that directly.
TEST_CASE("the wired planning leaves write the oracle's audit_log rows", "[cmd][handlers][audit][6100]") {
  auto const fx = make_fixture("auditrows");
  associate_cwd(fx, "acme");

  // `init` registers a project and writes NOTHING; the two rows below are
  // the association `create` and the `link` from `assoc add`. That
  // asymmetry is oracle-derived, not an omission.
  CHECK(audit_transcript(fx) == "create|association|1|<NULL>|<NULL>|create association 'acme'\n"
                                "link|association|1|<NULL>|<NULL>|add project 'proj' to association 'acme'\n");

  REQUIRE(dispatch(fx, {"plan", "create", "Plan One"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "update", "1", "--summary", "s"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Task One", "--plan", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Task Two", "--plan", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "block", "2", "--on", "1", "--reason", "waiting"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "cancel", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "reopen", "2", "--status", "todo", "--reason", "regressed"}).code == 0);

  CHECK(audit_transcript(fx) ==
        "create|association|1|<NULL>|<NULL>|create association 'acme'\n"
        "link|association|1|<NULL>|<NULL>|add project 'proj' to association 'acme'\n"
        "create|plan|1|<NULL>|<NULL>|create plan 'Plan One'\n"
        // A pure field patch is `update` with a NULL summary...
        "update|plan|1|<NULL>|<NULL>|<NULL>\n"
        "create|task|1|<NULL>|<NULL>|create task 'Task One'\n"
        "create|task|2|<NULL>|<NULL>|create task 'Task Two'\n"
        // ...and a generic `task update --status` is the ONE task
        // status_change with no summary text.
        "status_change|task|1|<NULL>|<NULL>|<NULL>\n"
        // The plan roll-up follows the task row, never precedes it, and
        // carries all five counters even at zero. The arrow is U+2192.
        "status_change|plan|1|<NULL>|<NULL>|recompute plan 1: draft → active; tasks todo=1 doing=1 blocked=0 done=0 "
        "cancelled=0\n"
        // The dedicated verbs each carry a summary: the STATUS word for
        // done/cancel, a sentence for block/reopen.
        "status_change|task|1|<NULL>|<NULL>|done\n"
        "status_change|task|2|<NULL>|<NULL>|blocked on task 1: waiting\n"
        "status_change|task|2|<NULL>|<NULL>|cancelled\n"
        "status_change|task|2|<NULL>|<NULL>|reopen to todo: regressed\n");
  // NOTE the four trailing task rows carry NO further plan row. Every one
  // of those four verbs DOES call `recompute_plan`; the plan is already
  // `active` and `compute_target` returns no change, so nothing is written.
  // The first draft of this case asserted two more `recompute plan` rows
  // here on the assumption that a recompute call implies a recompute row.
  // Running the identical argv against the Zig binary and diffing the two
  // transcripts is what corrected it -- the oracle emits exactly the
  // fourteen rows above.
}

// The arm that catches the whole class: a refused mutation must leave the
// table exactly as it found it. A `record` call placed BEFORE the mutation
// -- or outside the transaction that rolls the mutation back -- passes
// every "the row exists" assertion and fails only this one.
TEST_CASE("a refused mutation writes no audit_log row", "[cmd][handlers][audit][6100]") {
  auto const fx = make_fixture("auditrefuse");
  associate_cwd(fx, "acme");
  REQUIRE(dispatch(fx, {"plan", "create", "Plan One"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Task One", "--plan", "1"}).code == 0);

  // Land the task in a terminal status so an illegal transition is
  // reachable. (`task reopen` on a live task is NOT a refusal -- it is
  // force-gated by construction and exits 0. Discovered by running it.)
  REQUIRE(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "done", "1"}).code == 0);

  auto const before = audit_transcript(fx);
  REQUIRE_FALSE(before.empty());

  // Illegal transition, unknown scope, unknown status, missing row --
  // four different refusal paths through three different verbs.
  CHECK(dispatch(fx, {"task", "update", "1", "--status", "doing"}).code != 0);
  CHECK(dispatch(fx, {"plan", "create", "Nope", "--scope", "nosuchscope"}).code != 0);
  CHECK(dispatch(fx, {"task", "update", "1", "--status", "bogus"}).code != 0);
  CHECK(dispatch(fx, {"task", "update", "999", "--title", "ghost"}).code != 0);
  CHECK(dispatch(fx, {"plan", "update", "999", "--summary", "ghost"}).code != 0);

  CHECK(audit_transcript(fx) == before);

  // A patch that changes NOTHING is not a refusal -- it exits 0 -- and it
  // writes no row either. That is a separate arm from the four above and
  // the early-return that produces it is easy to move to the wrong side of
  // the audit call.
  REQUIRE(dispatch(fx, {"task", "update", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "update", "1"}).code == 0);
  CHECK(audit_transcript(fx) == before);
}

// -------------------------------------------------------------------------
// `<family> list` scope-vs-status validation ORDER (task 6200)
// -------------------------------------------------------------------------
//
// Ten cells: five families x {inside a registered scope, outside every
// registered scope}, each probed with `--status bogus`. They are pinned
// TOGETHER, in one case, because the property under test is a DISAGREEMENT
// between families and a per-family case cannot express it — split across
// five files, the next porter reads whichever one they opened and
// "harmonizes" the other four to match.
//
// The oracle carries TWO orderings and neither is derivable from the other:
//
//                       inside a scope        outside every scope
//   plan                unknown status        unknown status
//   question            unknown status        unknown status
//   scenario            unknown status        unknown status
//   task                unknown status        SCOPE error
//   decision            unknown status (2)    SCOPE error
//
// Captured from `zig/zig-out/bin/planar` in a pinned scratch arena, family
// by family, at task 6200. `decision`'s exit 2 in the left column is the
// oracle's too and is NOT a defect — see the `decision list` case below and
// `handlers/decision.cpp`'s note on task 6201.
//
// THE EMPTY STRING IS NOT A SUBSTITUTE FOR `bogus` HERE. `plan`,
// `question` and `scenario` comma-split `--status`, so `--status ""`
// produces zero tokens, skips their validator entirely, and makes all five
// families report the scope error from outside — which reads as "everyone
// resolves scope first" and is exactly the wrong conclusion. The
// state-differential lane runs only the empty-string shape and only from
// outside a scope, so it reported precisely that; see
// `statediff.t.cpp`'s note at its `--status ""` steps.

/// @brief Dispatch from a cwd that is registered nowhere, so the read-set
/// resolution fails. Mirrors the inline pattern the `plan list` empty-read-
/// set case above uses.
/// @param fx The fixture, already `init`ed.
/// @param args The argv tail.
/// @return The captured invocation.
namespace {
auto dispatch_outside_scope(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "elsewhere", ec);

  auto outside        = fx;
  outside.vars["PWD"] = (fx.root / "elsewhere").string();

  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(outside.vars), fx.root / "elsewhere", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

constexpr std::string_view k_scope_err = "error: cwd is not inside any registered Planar scope; cd into a registered "
                                         "scope or pass --scope global\n";
} // namespace

TEST_CASE("plan/question/scenario list validate --status BEFORE scope; task/decision resolve scope first",
          "[cmd][handlers][parity][6200]") {
  auto const fx = make_fixture("statusorder");
  associate_cwd(fx, "acme");

  // --- Inside a scope, all five report the status. This column is what
  // makes the right-hand column meaningful: it proves every family's
  // validator is reachable and rejects the token, so a scope error on the
  // right is an ORDERING difference and not a family that quietly ignores
  // `--status`.
  for (auto const& family : {"plan", "question", "scenario", "task", "decision"}) {
    CAPTURE(family);
    auto const got = dispatch(fx, {family, "list", "--status", "bogus"});
    CHECK(got.err == "error: unknown status 'bogus'\n");
    CHECK(got.out.empty());
    // Exit 1 everywhere EXCEPT `decision`, which exits 2. Oracle-captured
    // on both sides of the split; see the `decision list` case.
    CHECK(got.code == (std::string_view{family} == "decision" ? 2 : 1));
  }

  // --- Outside every registered scope, the two orderings separate.
  for (auto const& family : {"plan", "question", "scenario"}) {
    CAPTURE(family);
    auto const got = dispatch_outside_scope(fx, {family, "list", "--status", "bogus"});
    CHECK(got.err == "error: unknown status 'bogus'\n");
    CHECK(got.code == 1);
  }
  for (auto const& family : {"task", "decision"}) {
    CAPTURE(family);
    auto const got = dispatch_outside_scope(fx, {family, "list", "--status", "bogus"});
    CHECK(got.err == k_scope_err);
    CHECK(got.out.empty());
    // Exit 1 for BOTH, `decision` included: the scope error's code, not the
    // status error's. This is the cell task 6201 was filed against, and it
    // is 1 here because the reordering means the exit-2 arm is never
    // reached — not because that arm was changed.
    CHECK(got.code == 1);
  }
}

TEST_CASE("an EXPLICIT --scope keeps the status error ahead of the engine's SlugNotFound", "[cmd][handlers][parity][6200]") {
  // The other half of task 6200's reordering, and the reason it moved only
  // the cwd-derived branch. An explicit `--scope` never consults the read
  // set, so the status validator still runs first and beats the engine's
  // own slug resolution — `--scope nosuchslug --status bogus` reports the
  // STATUS on both families. Oracle-captured. Moving the whole scope
  // concern ahead of `--status` (the obvious spelling of the fix) would
  // report `SlugNotFound` here and trade one divergence for another.
  auto const fx = make_fixture("statusorderscope");
  associate_cwd(fx, "acme");

  auto const t = dispatch(fx, {"task", "list", "--scope", "nosuchslug", "--status", "bogus"});
  CHECK(t.err == "error: unknown status 'bogus'\n");
  CHECK(t.code == 1);

  auto const d = dispatch(fx, {"decision", "list", "--scope", "nosuchslug", "--status", "bogus"});
  CHECK(d.err == "error: unknown status 'bogus'\n");
  CHECK(d.code == 2);

  // ...and with a VALID status the same argv reaches the engine and gets
  // its SlugNotFound, proving the two errors really are ordered rather
  // than one of them being unreachable.
  CHECK(dispatch(fx, {"task", "list", "--scope", "nosuchslug", "--status", "todo"}).err == "error: task list: SlugNotFound\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "nosuchslug", "--status", "proposed"}).err ==
        "error: decision list: SlugNotFound\n");
}
