// @file handlers.t.cpp
// @brief In-process tests for the five ported verb handlers (plan 996,
// task 6105).
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
// Timestamps are the only fields not asserted literally — they are wall
// clock. Everything around them is.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cli;
import planar.engine.identity;
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
  auto const         tree  = planar::cmd::root_command();
  auto const         table = planar::cmd::handlers();
  int const          code  = planar::cmd::run(ctx, tree, table);
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

  // FINDING, pinned rather than papered over. `planar.cli.version`'s module
  // header claims the divergence preserves the field COUNT — "a script
  // splitting on whitespace still finds five tokens". It does not:
  // `compiler_version_string()` returns `Clang 22.1.8`, which contains a
  // space, so the line splits into SIX tokens where the oracle's splits
  // into five. The claim was written before the function was wired to a
  // real compiler string. This assertion records the actual behaviour so
  // the discrepancy is visible; fixing it means changing
  // `planar.cli.version` (a layer-1 module owned by an earlier task), which
  // is out of this task's scope and is filed separately.
  auto const tokens = std::ranges::count(got.out, ' ') + 1;
  CHECK(tokens == 6);
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
