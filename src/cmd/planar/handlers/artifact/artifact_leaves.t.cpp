// @file artifact_leaves.t.cpp
// @brief Leaf-level tests for the five ported `planar artifact` leaves
// (plan 996 roadmap M12 item 10, task 6196).
//
// Same shape as `question_leaves.t.cpp` / `decision_leaves.t.cpp` /
// `scenario_leaves.t.cpp`: dispatch the real tree and table against a
// scratch root, then assert BOTH the operator-visible output and the
// resulting DATABASE ROWS. Exit code 0 is not the contract — a verb that
// silently no-ops is still exit 0.
//
// ## Every expectation here came from RUNNING the oracle
//
// Not from reading zig, not from copying `scenario`. The five families'
// leaves look interchangeable and are not. Captured against
// `zig/zig-out/bin/planar` in a pinned scratch arena (`$PLANAR_DB`,
// `PLANAR_HOME`, `PLANAR_CONFIG_PATH`, `PLANAR_LOCAL_HOME` and `HOME` all
// redirected), and then re-verified by running the built C++ binary over
// the SAME 55 argv shapes in a fresh arena each and diffing stdout,
// stderr and exit code — all 55 identical. The transcript-worthy ones:
//
//   $Z artifact list                       b'(no artifacts)\n' -- parenthesised
//   $Z artifact list --scope global --kind nosuch   exit 2  b"error: unknown kind 'nosuch'\n"
//   $Z artifact list --scope global --status final  exit 2  b"error: unknown status 'final'\n"
//       ^ BOTH exit 2. The character-identical refusal on `scenario
//         list --status` exits 1. One flag name, two families, two codes.
//   $Z artifact show 999                   exit 1  b'error: no artifact with id 999\n'
//   $Z artifact list --scope nosuchslug    exit 1  b'error: artifact list: SlugNotFound\n'
//       ^ ONE family, TWO refusal FORMATS: prose for the single-id
//         lookup, verb-path-plus-Zig-tag for the scope failure.
//   $Z artifact add x --kind adr --plan 999  exit 1  b'error: artifact add: NotFound\n'
//       ^ and NOTHING is written. `scenario add --plan 4242` succeeds and
//         leaves a DANGLING edge. Opposite answers, same flag name.
//   $Z artifact update 1                   exit 1  b'error: at least one field must be specified for update\n'
//       ^ PROSE, and exit 1 -- not the `NoFields` tag, not exit 2.
//   $Z artifact add x --kind adr --body y --from-file z  exit 2
//       b'error: --body and --from-file are mutually exclusive\n'
//   $Z artifact add x --kind adr --body @missing   exit 2  b'error: read --body: FileNotFound\n'
//   $Z artifact add x --kind adr --from-file missing exit 2
//       b'error: read --from-file missing: FileNotFound\n'
//       ^ the `--from-file` refusal NAMES the path; the `--body` one does
//         not. Asymmetric in the oracle, reproduced.
//   $Z artifact add x --kind adr --editor  exit 0, silent
//       ^ `scenario add --editor` writes a `warning: --editor not yet
//         implemented` line to stderr. This family writes NOTHING.
//
// ## The empty `--status` filter means `{draft, active}`
//
// A FOURTH distinct answer from four sibling families. It is asserted
// below by seeding a `superseded` row and proving a bare list EXCLUDES it
// while an out-of-scope-of-the-filter row SURVIVES — an exclusion proved
// by what is missing AND by what remains, never by a shorter list alone.
//
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

/// @brief A scratch root plus the environment and database path every case
/// dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory.
///
/// Nothing here reads the real environment, so there is no path by which
/// the operator's `~/.planar/planar.db` can be reached.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_a_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
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

/// @brief Run one read-only query and render its rows as `a|b|c` lines
/// joined by `;`, with SQL NULL as the literal `<NULL>`.
///
/// The `<NULL>` spelling is the point: this family's `body` and
/// `source_path` are nullable, and an absent body (`<NULL>`) must stay
/// distinguishable from `--body ""` (the empty field). A projection that
/// rendered both as the empty string would pass while the two collapsed.
/// @param conn An open connection to the fixture database.
/// @param sql The query. A test-local literal, never operator input.
/// @param columns How many columns the projection selects.
/// @return The rendered rows, or the empty string when nothing matched.
auto query_rows(planar::db::connection& conn, std::string_view sql, int columns) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ';';
    }
    for (int col = 0; col < columns; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every `artifacts` row as
/// `id|scope_kind|scope_id|kind|title|body|source_path|status`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto artifact_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn, "select id, scope_kind, scope_id, kind, title, body, source_path, status from artifacts order by id",
                    8);
}

/// @brief The `audit_log` rows for one entity kind, as
/// `verb|entity_id|summary|actor|scope`.
///
/// `actor` and `scope` are included precisely because they are ALWAYS NULL
/// on the CLI path: a port that helpfully filled them in would be writing
/// rows the oracle does not.
/// @param conn An open connection to the fixture database.
/// @param kind The `entity_kind` to filter on.
/// @return The rendered rows, ascending by id.
auto audit_rows(planar::db::connection& conn, std::string_view kind) -> std::string {
  return query_rows(conn,
                    std::format("select verb, entity_id, summary, actor, scope from audit_log "
                                "where entity_kind = '{}' order by id",
                                kind),
                    5);
}

/// @brief Seed a project with an association and an anchor plan, so
/// association-scoped writes and `--plan` linkage are both reachable.
/// @param fx The fixture.
void seed_association_and_plan(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--json"}).code == 0);
}

} // namespace

// ===========================================================================
// artifact add
// ===========================================================================

TEST_CASE("artifact add writes the row, the audit row and the session", "[cmd][artifact][add]") {
  auto const fx = make_fixture("add");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto const res = dispatch(fx, {"artifact", "add", "T1", "--kind", "tech_spec", "--json"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  // `--status` defaults to `draft`, which is NOT the column default
  // (`active`). A port that let the column default apply would land
  // `active` here and still exit 0.
  //
  // With no association on the project the row lands `global` — NOT a
  // refusal. `plan create` refuses in this same state; the planning
  // families deliberately do not.
  CHECK(artifact_rows(conn) == "1|global|<NULL>|tech_spec|T1|<NULL>|<NULL>|draft");
  // The summary carries the KIND as well as the title, unlike
  // `scenario`'s, which is the title alone.
  CHECK(audit_rows(conn, "artifact") == "create|1|create artifact 'T1' (kind=tech_spec)|<NULL>|<NULL>");
}

TEST_CASE("artifact add distinguishes an absent body from an empty one", "[cmd][artifact][add]") {
  auto const fx = make_fixture("addbody");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"artifact", "add", "NoBody", "--kind", "other", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "EmptyBody", "--kind", "other", "--body", "", "--json"}).code == 0);

  auto conn = open_db(fx);
  // The whole point of the `<NULL>` spelling: row 1 has SQL NULL, row 2
  // has the empty string. They are different values and both are
  // operator-visible as `null` vs `""` in the JSON.
  CHECK(artifact_rows(conn) == "1|global|<NULL>|other|NoBody|<NULL>|<NULL>|draft;"
                               "2|global|<NULL>|other|EmptyBody||<NULL>|draft");

  auto const shown = dispatch(fx, {"artifact", "show", "1", "--json"});
  CHECK(shown.out.contains("\"body\":null"));
  auto const shown2 = dispatch(fx, {"artifact", "show", "2", "--json"});
  CHECK(shown2.out.contains("\"body\":\"\""));
}

TEST_CASE("artifact add --body @file reads RAW, front matter included", "[cmd][artifact][add][body]") {
  auto const fx = make_fixture("addfile");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  // The brief for this task said the STRIPPED body must reach the engine.
  // Probed against the oracle: it does not. `readBody` is a bare file read
  // and the front-matter block lands in the column intact. This test pins
  // the RAW bytes so a future cycle cannot quietly introduce stripping —
  // front-matter parsing belongs to the workbench/editflow round-trip,
  // which is a different path and one of the four leaves not ported here.
  auto const        path = fx.root / "proj" / "fm.md";
  std::ofstream     out{path};
  std::string const contents = "---\ntitle: FM\n---\n\nReal body here.\n";
  out << contents;
  out.close();

  // ABSOLUTE paths, deliberately: `read_body` resolves a relative `@path`
  // against the PROCESS cwd — which is what the oracle does too — and in
  // this harness the process cwd is the build directory, not the
  // fixture's. Under a real invocation the two coincide. Using a relative
  // path here would test the harness's cwd, not the verb.
  auto const abs = path.string();
  REQUIRE(dispatch(fx, {"artifact", "add", "FromBody", "--kind", "other", "--body", "@" + abs, "--json"}).code == 0);
  // `--from-file` reads the same bytes AND fills `source_path` with the
  // flag's value as GIVEN, un-absolutized. `--body @` leaves it NULL.
  REQUIRE(dispatch(fx, {"artifact", "add", "FromFile", "--kind", "other", "--from-file", abs, "--json"}).code == 0);

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, body, source_path from artifacts order by id", 3) ==
        std::format("1|{}|<NULL>;2|{}|{}", contents, contents, abs));
}

TEST_CASE("artifact add refuses the invalid enums at exit 2, writing nothing", "[cmd][artifact][add][refusal]") {
  auto const fx = make_fixture("addbad");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto const bad_kind = dispatch(fx, {"artifact", "add", "X", "--kind", "nosuch", "--json"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: unknown kind 'nosuch'\n");

  auto const bad_status = dispatch(fx, {"artifact", "add", "X", "--kind", "adr", "--status", "nosuch", "--json"});
  CHECK(bad_status.code == 2);
  CHECK(bad_status.err == "error: unknown status 'nosuch'\n");

  auto const both = dispatch(fx, {"artifact", "add", "X", "--kind", "adr", "--body", "y", "--from-file", "z", "--json"});
  CHECK(both.code == 2);
  CHECK(both.err == "error: --body and --from-file are mutually exclusive\n");

  // The `--body` refusal does NOT name the path; the `--from-file` one
  // does. Asymmetric in the oracle, reproduced rather than regularised.
  auto const missing_body = dispatch(fx, {"artifact", "add", "X", "--kind", "adr", "--body", "@nope.md", "--json"});
  CHECK(missing_body.code == 2);
  CHECK(missing_body.err == "error: read --body: FileNotFound\n");

  auto const missing_file = dispatch(fx, {"artifact", "add", "X", "--kind", "adr", "--from-file", "nope.md", "--json"});
  CHECK(missing_file.code == 2);
  CHECK(missing_file.err == "error: read --from-file nope.md: FileNotFound\n");

  auto conn = open_db(fx);
  CHECK(artifact_rows(conn).empty());
  CHECK(audit_rows(conn, "artifact").empty());
  // Every one of those refusals lands AHEAD of the session, so none was
  // written. `--scope nosuchslug` (below) lands BEHIND it and one IS.
  CHECK(query_rows(conn, "select count(*) from sessions", 1) == "0");
}

TEST_CASE("artifact add starts a session before the engine's own refusal", "[cmd][artifact][add][session]") {
  auto const fx = make_fixture("addsess");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto const res = dispatch(fx, {"artifact", "add", "X", "--kind", "adr", "--scope", "nosuchslug", "--json"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: artifact add: SlugNotFound\n");

  auto conn = open_db(fx);
  CHECK(artifact_rows(conn).empty());
  // The session is a COMMITTED SIDE EFFECT of a FAILED create. This is
  // what separates this family from `scenario add`, which starts no
  // session at all — a port that moved the session onto the success path
  // would leave no row here and still pass every stdout check.
  CHECK(query_rows(conn, "select count(*), vendor from sessions", 2) == "1|cli");

  // The successful lazy session creation is an observable side effect even
  // though the artifact create then refuses.
  CHECK(audit_rows(conn, "session") == "create|1|start session vendor=cli|<NULL>|<NULL>");
}

TEST_CASE("artifact add --plan checks the plan and writes the edge", "[cmd][artifact][add][plan]") {
  auto const fx = make_fixture("addplan");
  seed_association_and_plan(fx);

  REQUIRE(dispatch(fx, {"artifact", "add", "Linked", "--kind", "tech_spec", "--plan", "1", "--json"}).code == 0);

  auto conn = open_db(fx);
  // `from_kind` is the bare `artifact` and the relationship is
  // `derives-from`. That spelling is what editflow's anchor resolver
  // queries, so a port that wrote a different one would break the
  // drafting quartet the day it lands.
  CHECK(query_rows(conn, "select from_kind, from_id, to_kind, to_id, relationship from entity_links", 5) ==
        "artifact|1|plan|1|derives-from");
  // Exactly ONE audit row, and its verb is `create` — the edge does NOT
  // get its own `link` row.
  CHECK(audit_rows(conn, "artifact") == "create|1|create artifact 'Linked' (kind=tech_spec)|<NULL>|<NULL>");

  // A nonexistent plan REFUSES and writes NOTHING — not the artifact, not
  // the audit row, not the edge. `scenario add --plan 4242` succeeds and
  // leaves a dangling edge; the two families genuinely disagree here.
  auto const bad = dispatch(fx, {"artifact", "add", "Dangling", "--kind", "adr", "--plan", "999", "--json"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: artifact add: NotFound\n");
  CHECK(query_rows(conn, "select count(*) from artifacts", 1) == "1");
  CHECK(query_rows(conn, "select count(*) from entity_links", 1) == "1");
}

TEST_CASE("artifact add --editor writes nothing to stderr", "[cmd][artifact][add][editor]") {
  auto const fx = make_fixture("addeditor");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  auto const res = dispatch(fx, {"artifact", "add", "E", "--kind", "other", "--editor", "--json"});
  CHECK(res.code == 0);
  // `scenario add --editor` writes `warning: --editor not yet
  // implemented; falling back to inline create`. This family writes
  // NOTHING — reproducing scenario's warning would put a line on stderr
  // the oracle never emits.
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select body from artifacts", 1) == "<NULL>");
}

// ===========================================================================
// artifact show
// ===========================================================================

TEST_CASE("artifact show renders both forms and refuses a missing id", "[cmd][artifact][show]") {
  auto const fx = make_fixture("show");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "T2", "--kind", "adr", "--body", "hello body", "--json"}).code == 0);

  // Labels pad to THIRTEEN columns; `scenario show`'s pad to twelve. The
  // `body:` line is CONDITIONAL and sits between `scope:` and `created:`.
  auto const text = dispatch(fx, {"artifact", "show", "1"});
  CHECK(text.code == 0);
  CHECK(text.out.contains("id:          1\n"));
  CHECK(text.out.contains("title:       T2\n"));
  CHECK(text.out.contains("kind:        adr\n"));
  CHECK(text.out.contains("status:      draft\n"));
  CHECK(text.out.contains("scope:       global\n"));
  CHECK(text.out.contains("body:        hello body\n"));

  // The prose refusal, NOT the `artifact show: NotFound` tag shape. This
  // family uses both formats and the split is by verb shape.
  auto const missing = dispatch(fx, {"artifact", "show", "999", "--json"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no artifact with id 999\n");
}

TEST_CASE("artifact show omits the body line when the column is NULL", "[cmd][artifact][show]") {
  auto const fx = make_fixture("shownobody");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "NoBody", "--kind", "other", "--json"}).code == 0);

  auto const text = dispatch(fx, {"artifact", "show", "1"});
  CHECK(text.code == 0);
  // Absent entirely, not rendered as an empty value — the line is
  // conditional on the column being non-NULL.
  CHECK_FALSE(text.out.contains("body:"));
  CHECK(text.out.contains("scope:       global\n"));
  CHECK(text.out.contains("created:"));
}

// ===========================================================================
// artifact list
// ===========================================================================

TEST_CASE("artifact list's empty status filter means {draft, active}", "[cmd][artifact][list][status]") {
  auto const fx = make_fixture("liststatus");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "D", "--kind", "other", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "A", "--kind", "other", "--status", "active", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "S", "--kind", "other", "--status", "superseded", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "R", "--kind", "other", "--status", "retired", "--json"}).code == 0);

  // THE copy-prone value. `question`'s empty arm means `open`,
  // `decision`'s `{proposed, accepted}`, `scenario`'s EVERY status. This
  // family's is `{draft, active}`.
  //
  // The exclusion is proved from BOTH sides: the two terminal rows are
  // absent AND the two non-terminal rows SURVIVE. A test that only checked
  // "S is missing" would also pass against a list that returned nothing.
  auto const bare = dispatch(fx, {"artifact", "list", "--scope", "global", "--json"});
  CHECK(bare.code == 0);
  CHECK(bare.out.contains("\"title\":\"D\""));
  CHECK(bare.out.contains("\"title\":\"A\""));
  CHECK_FALSE(bare.out.contains("\"title\":\"S\""));
  CHECK_FALSE(bare.out.contains("\"title\":\"R\""));

  // `--status ""` yields NO tokens, which lands the SAME `{draft, active}`
  // arm as omitting the flag — it is neither "match nothing" nor "match
  // everything". `--kind ""` differs: an empty kind set emits no predicate
  // at all. Two flags on one leaf, two meanings for the empty string.
  //
  // This assertion was originally written the other way round, from a
  // probe taken against a fixture that happened to hold NO terminal rows —
  // where "no predicate" and "{draft, active}" are indistinguishable. The
  // re-probe with `superseded` and `retired` rows present is what
  // separated them.
  auto const empty_filter = dispatch(fx, {"artifact", "list", "--scope", "global", "--status", "", "--json"});
  CHECK(empty_filter.code == 0);
  CHECK(empty_filter.out.contains("\"title\":\"D\""));
  CHECK(empty_filter.out.contains("\"title\":\"A\""));
  CHECK_FALSE(empty_filter.out.contains("\"title\":\"S\""));
  CHECK_FALSE(empty_filter.out.contains("\"title\":\"R\""));

  // Comma-split, and naming a terminal status reaches it.
  auto const named = dispatch(fx, {"artifact", "list", "--scope", "global", "--status", "superseded,retired", "--json"});
  CHECK(named.code == 0);
  CHECK(named.out.contains("\"title\":\"S\""));
  CHECK(named.out.contains("\"title\":\"R\""));
  CHECK_FALSE(named.out.contains("\"title\":\"D\""));
  CHECK_FALSE(named.out.contains("\"title\":\"A\""));
}

TEST_CASE("artifact list's kind filter is comma-split and excludes", "[cmd][artifact][list][kind]") {
  auto const fx = make_fixture("listkind");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "TS", "--kind", "tech_spec", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "AD", "--kind", "adr", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "OT", "--kind", "other", "--json"}).code == 0);

  auto const one = dispatch(fx, {"artifact", "list", "--scope", "global", "--kind", "adr", "--json"});
  CHECK(one.code == 0);
  CHECK(one.out.contains("\"title\":\"AD\""));
  // Proving EXCLUSION needs an out-of-filter row to be shown absent while
  // the in-filter row survives.
  CHECK_FALSE(one.out.contains("\"title\":\"TS\""));
  CHECK_FALSE(one.out.contains("\"title\":\"OT\""));

  auto const two = dispatch(fx, {"artifact", "list", "--scope", "global", "--kind", "tech_spec,adr", "--json"});
  CHECK(two.code == 0);
  CHECK(two.out.contains("\"title\":\"TS\""));
  CHECK(two.out.contains("\"title\":\"AD\""));
  CHECK_FALSE(two.out.contains("\"title\":\"OT\""));

  // `--kind ""` is NO predicate, not "match nothing".
  auto const empty = dispatch(fx, {"artifact", "list", "--scope", "global", "--kind", "", "--json"});
  CHECK(empty.code == 0);
  CHECK(empty.out.contains("\"title\":\"TS\""));
  CHECK(empty.out.contains("\"title\":\"OT\""));

  // Exit 2 — the character-identical refusal on `scenario list --status`
  // exits 1.
  auto const bad = dispatch(fx, {"artifact", "list", "--scope", "global", "--kind", "nosuch", "--json"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: unknown kind 'nosuch'\n");
}

TEST_CASE("artifact list's scope filter is a comma-split read set", "[cmd][artifact][list][scope]") {
  auto const fx = make_fixture("listscope");
  seed_association_and_plan(fx);
  REQUIRE(dispatch(fx, {"artifact", "add", "G", "--kind", "other", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "A", "--kind", "other", "--json"}).code == 0);

  auto conn = open_db(fx);
  // The second landed `association` because the project's repo path is now
  // registered under one — the SAME command lands `global` before
  // `assoc add`. Both captured in one arena.
  CHECK(query_rows(conn, "select id, scope_kind from artifacts order by id", 2) == "1|global;2|association");

  auto const only_global = dispatch(fx, {"artifact", "list", "--scope", "global", "--json"});
  CHECK(only_global.out.contains("\"title\":\"G\""));
  CHECK_FALSE(only_global.out.contains("\"title\":\"A\""));

  auto const both = dispatch(fx, {"artifact", "list", "--scope", "global,project:proj", "--json"});
  CHECK(both.code == 0);
  CHECK(both.out.contains("\"title\":\"G\""));
  CHECK(both.out.contains("\"title\":\"A\""));

  // An unresolvable member fails the WHOLE call rather than being skipped:
  // a read verb that drops one member returns a short list that looks
  // complete. Note the tag shape, not the prose shape.
  auto const bad = dispatch(fx, {"artifact", "list", "--scope", "global,nosuchslug", "--json"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: artifact list: SlugNotFound\n");
}

TEST_CASE("artifact list's plan filter intersects with scope", "[cmd][artifact][list][plan]") {
  auto const fx = make_fixture("listplan");
  seed_association_and_plan(fx);
  REQUIRE(dispatch(fx, {"artifact", "add", "Linked", "--kind", "other", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Loose", "--kind", "other", "--json"}).code == 0);

  auto const filtered = dispatch(fx, {"artifact", "list", "--plan", "1", "--json"});
  CHECK(filtered.code == 0);
  CHECK(filtered.out.contains("\"title\":\"Linked\""));
  CHECK_FALSE(filtered.out.contains("\"title\":\"Loose\""));

  // It INTERSECTS with the scope predicate rather than replacing it: the
  // linked artifact is association-scoped, so restricting to `global`
  // yields nothing even though the plan edge matches.
  auto const crossed = dispatch(fx, {"artifact", "list", "--scope", "global", "--plan", "1", "--json"});
  CHECK(crossed.code == 0);
  CHECK(crossed.out == "[]\n");
}

TEST_CASE("artifact list renders the empty and populated tables", "[cmd][artifact][list][render]") {
  auto const fx = make_fixture("listrender");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  // Parenthesised, matching `question` and `scenario` and NOT `decision`'s
  // bare `no decisions`.
  auto const empty = dispatch(fx, {"artifact", "list", "--scope", "global"});
  CHECK(empty.code == 0);
  CHECK(empty.out == "(no artifacts)\n");
  CHECK(dispatch(fx, {"artifact", "list", "--scope", "global", "--json"}).out == "[]\n");

  REQUIRE(dispatch(fx, {"artifact", "add", "Scoped", "--kind", "adr", "--json"}).code == 0);
  // id right-aligned to 5, status left to 10, kind left to 16, then title.
  // `kind` occupies the column `scenario`'s table spends on `outcome`.
  CHECK(dispatch(fx, {"artifact", "list", "--scope", "global"}).out == "    1  draft       adr               Scoped\n");
}

// ===========================================================================
// artifact update
// ===========================================================================

TEST_CASE("artifact update refuses when no field is given", "[cmd][artifact][update]") {
  auto const fx = make_fixture("updatenofields");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "T", "--kind", "other", "--json"}).code == 0);

  auto const res = dispatch(fx, {"artifact", "update", "1", "--json"});
  // PROSE and exit 1 — not the `NoFields` tag, and not exit 2 the way the
  // invalid-enum refusals are.
  CHECK(res.code == 1);
  CHECK(res.err == "error: at least one field must be specified for update\n");

  auto conn = open_db(fx);
  CHECK(audit_rows(conn, "artifact") == "create|1|create artifact 'T' (kind=other)|<NULL>|<NULL>");
}

TEST_CASE("artifact update walks the status transition matrix", "[cmd][artifact][update][status]") {
  auto const fx = make_fixture("updatestatus");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "T", "--kind", "other", "--json"}).code == 0);

  // draft -> superseded is NOT an edge: a draft must be ACTIVATED before
  // it can be laid to rest. `scenario`'s `draft -> retired` shortcut has
  // no counterpart here.
  auto const illegal = dispatch(fx, {"artifact", "update", "1", "--status", "superseded", "--json"});
  CHECK(illegal.code == 1);
  CHECK(illegal.err == "error: artifact update: IllegalTransition\n");

  auto conn = open_db(fx);
  // The refused transition wrote NOTHING — not the status, not an audit
  // row. A port that recorded the attempt would still exit 1 and pass a
  // stderr-only check.
  CHECK(query_rows(conn, "select status from artifacts where id = 1", 1) == "draft");
  CHECK(audit_rows(conn, "artifact") == "create|1|create artifact 'T' (kind=other)|<NULL>|<NULL>");

  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--status", "active", "--json"}).code == 0);
  // active -> draft IS legal. No sibling family lets its second state walk
  // back to its first.
  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--status", "draft", "--json"}).code == 0);
  CHECK(query_rows(conn, "select status from artifacts where id = 1", 1) == "draft");

  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--status", "active", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--status", "retired", "--json"}).code == 0);
  // `retired` is terminal — every outgoing edge is refused...
  CHECK(dispatch(fx, {"artifact", "update", "1", "--status", "active", "--json"}).code == 1);
  // ...but the IDENTITY move succeeds, because `check_transition`
  // short-circuits on `from == to` before consulting the matrix.
  CHECK(dispatch(fx, {"artifact", "update", "1", "--status", "retired", "--json"}).code == 0);

  // A status move records `status_change`, not `update`, and its summary
  // is genuinely NULL — unlike `create`, which carries prose.
  CHECK(query_rows(conn, "select verb, summary from audit_log where entity_kind = 'artifact' and verb = 'status_change' limit 1",
                   2) == "status_change|<NULL>");
}

TEST_CASE("artifact update patches fields and bumps updated_at", "[cmd][artifact][update]") {
  auto const fx = make_fixture("updatefields");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Before", "--kind", "other", "--json"}).code == 0);

  auto       conn   = open_db(fx);
  auto const before = query_rows(conn, "select updated_at from artifacts where id = 1", 1);

  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--title", "After", "--source-path", "docs/x.md", "--json"}).code == 0);
  CHECK(query_rows(conn, "select title, source_path from artifacts where id = 1", 2) == "After|docs/x.md");
  CHECK(query_rows(conn, "select updated_at from artifacts where id = 1", 1) != before);

  // A field-only update records `update`, not `status_change`.
  CHECK(query_rows(conn, "select verb, summary from audit_log where entity_kind = 'artifact' and verb = 'update'", 2) ==
        "update|<NULL>");
}

TEST_CASE("artifact update resolves the scope slug before the id lookup", "[cmd][artifact][update][scope]") {
  auto const fx = make_fixture("updateorder");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);

  // Both the id AND the slug are bad. The oracle reports the SLUG failure,
  // which is only possible if scope resolution runs ahead of the existence
  // check. An implementation that looked the row up first would report
  // `no artifact with id 999` and still exit 1.
  auto const res = dispatch(fx, {"artifact", "update", "999", "--scope", "nosuchslug", "--json"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: artifact update: SlugNotFound\n");

  // ...and with a GOOD slug, the missing id surfaces normally.
  auto const missing = dispatch(fx, {"artifact", "update", "999", "--scope", "global", "--json"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no artifact with id 999\n");
}

TEST_CASE("artifact update --body @file reads RAW too", "[cmd][artifact][update][body]") {
  auto const fx = make_fixture("updatebody");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "T", "--kind", "other", "--json"}).code == 0);

  auto const        path = fx.root / "proj" / "wrapped.md";
  std::ofstream     out{path};
  std::string const contents = "---\nentity_kind: artifact\n---\n\nwrapped body\n";
  out << contents;
  out.close();

  // Absolute, for the same harness-cwd reason as the `add` case above.
  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--body", "@" + path.string(), "--json"}).code == 0);

  auto conn = open_db(fx);
  // The front matter is STILL THERE. This is the assertion the task brief
  // predicted would go the other way; it pins the observed behaviour so a
  // stripping step cannot be added on the assumption that it should.
  CHECK(query_rows(conn, "select body from artifacts where id = 1", 1) == contents);
}

// ===========================================================================
// artifact link
// ===========================================================================

TEST_CASE("artifact link writes the edge through the shared surface", "[cmd][artifact][link]") {
  auto const fx = make_fixture("link");
  seed_association_and_plan(fx);
  REQUIRE(dispatch(fx, {"artifact", "add", "A", "--kind", "other", "--json"}).code == 0);

  auto const res = dispatch(fx, {"artifact", "link", "1", "plan:1", "--relationship", "cites", "--json"});
  CHECK(res.code == 0);
  // The JSON key is `artifact_id`, and the subject kind on the edge is the
  // bare `artifact`.
  CHECK(res.out.contains("\"artifact_id\":1"));
  CHECK(res.out.contains("\"relationship\":\"cites\""));

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select from_kind, from_id, to_kind, to_id, relationship from entity_links", 5) ==
        "artifact|1|plan|1|cites");
}

TEST_CASE("artifact link requires --relationship before checking the subject", "[cmd][artifact][link][refusal]") {
  auto const fx = make_fixture("linkrel");
  seed_association_and_plan(fx);
  REQUIRE(dispatch(fx, {"artifact", "add", "A", "--kind", "other", "--json"}).code == 0);

  auto const missing = dispatch(fx, {"artifact", "link", "1", "plan:1", "--json"});
  CHECK(missing.code == 2);
  CHECK(missing.err == "error: --relationship is required\n");

  // The ordering is observable: a NONEXISTENT subject still reports the
  // missing relationship, because the flag check runs ahead of endpoint
  // existence.
  auto const bad_subject = dispatch(fx, {"artifact", "link", "999", "plan:1", "--json"});
  CHECK(bad_subject.code == 2);
  CHECK(bad_subject.err == "error: --relationship is required\n");

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select count(*) from entity_links", 1) == "0");
}

TEST_CASE("document authority derives and validates every adjacent Unicode passage", "[cmd][document][range]") {
  auto const fx = make_fixture("doc_range");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Canonical", "--kind", "tech_spec", "--body",
                        "# Heading\nFirst paragraph\n- middle item\nUnicode café ☕", "--json"})
              .code == 0);

  auto const projected = dispatch(fx, {"document", "project", "--kind", "artifact", "--id", "1", "--json"});
  REQUIRE(projected.code == 0);
  CHECK(projected.out.contains("\"contract_version\":\"block-document-v1\""));
  CHECK(projected.out.contains("\"text\":\"Unicode café ☕\""));

  auto field = [&](std::string_view marker, std::size_t from = 0) {
    auto begin = projected.out.find(marker, from);
    REQUIRE(begin != std::string::npos);
    begin += marker.size();
    auto end = projected.out.find('"', begin);
    REQUIRE(end != std::string::npos);
    return std::pair{projected.out.substr(begin, end - begin), end};
  };
  auto const [revision, revision_end]       = field("\"content_revision\":\"");
  auto const [title_key, title_end]         = field("\"key\":\"", revision_end);
  auto const [heading_key, heading_end]     = field("\"key\":\"", title_end);
  auto const [paragraph_key, paragraph_end] = field("\"key\":\"", heading_end);
  auto const [list_key, list_end]           = field("\"key\":\"", paragraph_end);
  auto const [unicode_key, ignored]         = field("\"key\":\"", list_end);

  auto const valid = dispatch(fx, {"document",
                                   "validate-range",
                                   "--kind",
                                   "artifact",
                                   "--id",
                                   "1",
                                   "--content-revision",
                                   revision,
                                   "--start-key",
                                   heading_key,
                                   "--start-offset",
                                   "0",
                                   "--end-key",
                                   unicode_key,
                                   "--end-offset",
                                   "17",
                                   "--covered-key",
                                   heading_key,
                                   "--covered-key",
                                   paragraph_key,
                                   "--covered-key",
                                   list_key,
                                   "--covered-key",
                                   unicode_key,
                                   "--segment-quote",
                                   "Heading",
                                   "--segment-quote",
                                   "First paragraph",
                                   "--segment-quote",
                                   "middle item",
                                   "--segment-quote",
                                   "Unicode café ☕",
                                   "--json"});
  CHECK(valid.code == 0);
  CHECK(valid.out.contains("\"normalized_quote\":\"Heading\\nFirst paragraph\\nmiddle item\\nUnicode café ☕\""));

  auto omitted = dispatch(fx, {"document",
                               "validate-range",
                               "--kind",
                               "artifact",
                               "--id",
                               "1",
                               "--content-revision",
                               revision,
                               "--start-key",
                               heading_key,
                               "--start-offset",
                               "0",
                               "--end-key",
                               unicode_key,
                               "--end-offset",
                               "17",
                               "--covered-key",
                               heading_key,
                               "--covered-key",
                               unicode_key,
                               "--segment-quote",
                               "Heading",
                               "--segment-quote",
                               "Unicode café ☕",
                               "--json"});
  CHECK(omitted.code != 0);
  CHECK(omitted.err.contains("noncontiguous_covered_keys"));

  auto mid_codepoint = dispatch(
      fx,
      {"document",      "validate-range", "--kind",          "artifact", "--id",      "1",         "--content-revision", revision,
       "--start-key",   unicode_key,      "--start-offset",  "12",       "--end-key", unicode_key, "--end-offset",       "13",
       "--covered-key", unicode_key,      "--segment-quote", "",         "--json"});
  CHECK(mid_codepoint.code != 0);
  CHECK(mid_codepoint.err.contains("invalid_utf8_boundary"));

  auto stale = dispatch(fx, {"document",           "validate-range", "--kind",       "artifact", "--id",           "1",
                             "--content-revision", "forged",         "--start-key",  title_key,  "--start-offset", "0",
                             "--end-key",          title_key,        "--end-offset", "9",        "--covered-key",  title_key,
                             "--segment-quote",    "Canonical",      "--json"});
  CHECK(stale.code != 0);
  CHECK(stale.err.contains("stale_revision"));
}
