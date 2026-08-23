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

import std;
import planar.cli;
import planar.db;
import planar.engine.external;
import planar.engine.identity;
import planar.db.migrate;
import planar.engine.workbench.fsutil;
import planar.cmd.planar.args;
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

// =========================================================================
// Task 6106 — the leaves that were waiting for this layer.
// =========================================================================

TEST_CASE("parse_int64_zig reproduces std.fmt.parseInt, separators included", "[cmd][args][parity]") {
  using planar::cmd::parse_int64_zig;

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
  // A childless node is a LEAF here (planar.cli.cmd), so without a handler
  // this verb would answer `error: not implemented yet` and exit 64. The
  // oracle answers exit 0 and a help page; these bytes are that page.
  auto const fx  = make_fixture("skills");
  auto const got = dispatch(fx, {"skills"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);
  CHECK(got.out == "skills\n"
                   "\n"
                   "The unified skill source tree under skills/src/ is rendered by the\n"
                   "  external scriptorium binary (plan 918). Planar no longer renders vendor\n"
                   "  projections nor tracks their install-drift in-band; use `scriptorium\n"
                   "  check`/`scriptorium status` instead. This command has no subcommands.\n"
                   "\n"
                   "USAGE:\n"
                   "  skills\n");

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
  CHECK(got.code == 2);
  CHECK(got.out == "error: too many positional arguments (got extra)\n");
  CHECK(got.err == "error: TooManyPositionals\n");
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

TEST_CASE("the gc drift refusal exits 1 with NOTHING printed", "[cmd][handlers][workbench][oracle-defect]") {
  // Reproduces an ORACLE DEFECT deliberately (task 6122): the Zig handler
  // writes its refusal to the buffered ctx.stderr and then calls
  // std.process.exit(1) directly, skipping the runtime shutdown that would
  // flush it. Verified against the oracle with stderr on a terminal.
  // `render_cli::render_gc_drift_refusal` holds the intended text and is
  // pinned in render_cli.t.cpp, ready to wire the moment the divergence is
  // sanctioned.
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
  CHECK(refused.out.empty());
  CHECK(refused.err.empty());
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
