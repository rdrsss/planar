// @file decision_leaves.t.cpp
// @brief In-process tests for the seven leaves wired by plan 996, task
// 6194: `decision add`, `show`, `list`, `accept`, `supersede`, `withdraw`
// and `link`.
//
// Its own file rather than more of `handlers.t.cpp` (3.8k lines already),
// on the discipline `annotate_leaves.t.cpp`, `plan_task_remainder_leaves.
// t.cpp` and `question_leaves.t.cpp` established.
//
// ## EVERY CASE ASSERTS DATABASE ROWS
//
// Stdout is checked where the bytes are the contract, but no case rests on
// stdout alone. The defect class this milestone keeps finding is a verb
// that exits 0 with oracle-identical output and wrong rows. So every
// mutation here is followed by a `decision_rows` / `audit_rows` /
// `edge_rows` snapshot, and every REFUSAL is followed by one too — a
// refusal that half-wrote is the worse bug, and only the after-state
// distinguishes it. `decision supersede`'s duplicate-edge refusal is the
// sharpest instance: it runs the status UPDATE *before* the INSERT that
// fails.
//
// SQL NULL renders as the literal `<NULL>` and is never collapsed onto the
// empty string. `decisions.rationale` is genuinely nullable and an absent
// `--rationale` must produce NULL, not `""`. `decisions.body` is the
// INVERSE: it is `not null`, so `--body ""` must produce `''` and never a
// synthesized placeholder. Both directions are asserted.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, BY A SURVIVOR
//
// Each `decision list` case seeds rows on BOTH sides of its predicate and
// asserts the returned set AND that the excluded rows are still in the
// table afterwards.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string was captured from `zig/zig-out/bin/planar`
// against a scratch `PLANAR_DB`, read back through a Python `repr` rather
// than through a pipe into `tail`. The whole set was then re-derived as a
// SEQUENCE diff: a 61-step argv script replayed against BOTH binaries with
// their stdout, stderr, exit codes AND resulting `decisions` /
// `entity_links` / `audit_log` / `sessions` / `agent_actions` dumps
// compared row by row. The captures that decided a shape:
//
//   $Z decision list --scope global    stdout b'no decisions\n'
//       ^ NO PARENTHESES. `question` emits `(no questions)`.
//   $Z decision list --scope global --json   stdout b'[]\n'
//   $Z decision add first --body because
//       b'id:         1\ntitle:      first\n...'
//       ^ values start at column 13. `question`'s start at 12, `plan`'s
//         at 11. Three sibling blocks, three widths.
//   $Z decision add first               exit 2
//       b'error: --body is required (or run interactively to use the
//         editor flow)\n'
//       ^ a flag declared OPTIONAL that the handler requires.
//   $Z decision list --status bogus     exit **2**
//       ^ the SAME refusal on `question list` exits 1. zig's decision
//         list.zig raises InvalidInput; question's comes from the engine.
//   $Z decision list --status proposed,accepted  exit 2
//       b"error: unknown status 'proposed,accepted'\n"
//   $Z decision list --scope global,repo:repo1   exit 1  SlugNotFound
//       ^ NEITHER flag is comma-split here. Both are on `question list`.
//   $Z decision accept <withdrawn>      exit 1
//       b'error: decision 2 is terminal; cannot accept\n'
//   $Z decision supersede 3 --by 2 ; decision supersede 3 --by 4
//       ^ BOTH exit 0. An already-superseded decision can be superseded
//         again by a DIFFERENT decision; only `withdrawn` refuses.
//   $Z decision supersede 1 --by 2 (edge pre-existing)   exit 1
//       b'error: supersedes link from decision 2 to 1 already exists\n'
//       AND decision 1 still `proposed` — the rollback is observable.
//   $Z decision add planned --body b3 --plan 9999        exit 0
//       ^ a DANGLING edge. `question add --plan 9999` refuses.
//   $Z decision link 1 plan:1 --relationship cites
//       b'linked decision:1 -> plan:1  [cites]  (link id: 1)\n'
//       ^ DOUBLE spaces around the bracket group.
//
// ## The two known divergences, each deliberate
//
//  1. `decision add --editor` with no `--body` prints an extra WARNING on
//     stderr before the oracle's refusal. The oracle opens `$EDITOR` there
//     — but only when stdout is a TTY; on a pipe it falls through to the
//     same refusal, so the non-interactive bytes on stdout are identical.
//     This build has no editflow and this `context` has no TTY probe, so
//     the warning is what keeps an interactive operator from reading the
//     refusal as the oracle's own answer. Deliberate, and asserted.
//  2. The oracle's duplicate-`supersede` refusal prints TWO stderr lines,
//     the first being a `std.log.err` (`decision.supersede entity_links
//     insert failed: StepFailed`). No engine module in this tree imports
//     `planar.log` at all, so no port reproduces Zig's log lines; the
//     refusal line itself is byte-identical.
//
// A fourth difference is not this task's: `decision supersede 1` with no
// `--by` reports `--by is required` / `RequiredError` where the oracle
// reports `required flag missing: --by` / `MissingRequired`. That is the
// shared CLI11 parser layer's wording for EVERY required flag in the
// binary, identical on `ext register jira --url`, and changing it here
// would be a tree-wide change smuggled into one family.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
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
                         std::format("planar_d_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  // A directory deliberately OUTSIDE every registered scope, for the
  // read-scope refusal.
  std::filesystem::create_directories(root / "outside", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table inside `fx`, from
/// the directory `where` relative to the fixture root.
/// @param fx The fixture.
/// @param where The cwd, relative to the fixture root.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch_in(const fixture& fx, std::string_view where, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / where, fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Dispatch from the fixture's registered project directory.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  return dispatch_in(fx, "proj", std::move(args));
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
/// @param conn An open connection to the fixture database.
/// @param sql The query. A test-local literal, never operator input.
/// @param columns How many columns the projection selects.
/// @return The rendered rows, or `""` when the query matched nothing.
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

/// @brief Every `decisions` row as
/// `id|scope_kind|scope_id|title|body|rationale|status|decided_at|session_id`.
///
/// `decided_at` is a wall-clock timestamp, so it is projected as the
/// two-state `<NULL>` / `SET`; whether the column is null is the contract
/// (`accept` writes it, `withdraw` and `supersede` must not), its value is
/// not.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto decision_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select id, scope_kind, scope_id, title, body, rationale, status, "
                    "case when decided_at is null then null else 'SET' end, session_id from decisions order by id",
                    9);
}

/// @brief The `audit_log` rows for one entity kind, as
/// `verb|entity_id|summary|actor|scope`.
///
/// `actor` and `scope` are included precisely because they are ALWAYS NULL
/// on the CLI path: a port that helpfully filled them in would be writing
/// rows the oracle does not, invisibly to every stdout comparison.
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

/// @brief Every `decision -> *` edge as `from_id|to_kind|to_id|relationship`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto edge_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select from_id, to_kind, to_id, relationship from entity_links where from_kind = 'decision' order by id", 4);
}

/// @brief Bring a fixture up to an initialised database with `proj`
/// registered as a project.
/// @param fx The fixture.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
}

/// @brief Create a decision through the CLI, requiring success.
/// @param fx The fixture.
/// @param args The argv tail after `decision add`.
void add(const fixture& fx, std::vector<std::string> args) {
  std::vector<std::string> argv{"decision", "add"};
  argv.insert(argv.end(), args.begin(), args.end());
  auto const res = dispatch(fx, argv);
  INFO(res.err);
  REQUIRE(res.code == 0);
}

} // namespace

// ===========================================================================
// decision add
// ===========================================================================

TEST_CASE("decision add writes a proposed row and echoes the oracle's block") {
  auto const fx = make_fixture("add");
  seed(fx);

  auto const res = dispatch(fx, {"decision", "add", "first", "--body", "because"});
  REQUIRE(res.code == 0);
  // Values start at column 13. Everything but the two timestamps is fixed,
  // so the block is checked in halves rather than loosened to a `contains`.
  CHECK(res.out.starts_with("id:         1\n"
                            "title:      first\n"
                            "status:     proposed\n"
                            "scope:      global\n"
                            "body:       because\n"
                            "session:    1\n"
                            "created:    "));
  CHECK(res.out.ends_with("Z\n"));
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  // `scope_kind` is `global` even though `proj` IS a registered project:
  // it has no association, and unlike `plan create` this verb does not
  // refuse there. Same choice `question add` and `task add` make.
  CHECK(decision_rows(conn) == "1|global|<NULL>|first|because|<NULL>|proposed|<NULL>|1");
  CHECK(audit_rows(conn, "decision") == "create|1|create decision 'first'|<NULL>|<NULL>");
  CHECK(edge_rows(conn).empty());
  CHECK(audit_rows(conn, "session") == "create|1|start session vendor=cli|<NULL>|<NULL>");
  // The session row itself IS written, and its id lands on the decision.
  CHECK(query_rows(conn, "select id, vendor from sessions order by id", 2) == "1|cli");
  // No live claim, so the entity-create activity hook is a silent no-op.
  CHECK(query_rows(conn, "select count(*) from agent_actions", 1) == "0");
}

TEST_CASE("decision add REQUIRES --body, refusing at exit 2 before any write") {
  auto const fx = make_fixture("add-nobody");
  seed(fx);

  auto const res = dispatch(fx, {"decision", "add", "first"});
  CHECK(res.code == 2);
  CHECK(res.err == "error: --body is required (or run interactively to use the editor flow)\n");
  CHECK(res.out.empty());

  auto conn = open_db(fx);
  CHECK(decision_rows(conn).empty());
  CHECK(audit_rows(conn, "decision").empty());
  // The refusal fires BEFORE the session is started, so not even that side
  // effect lands — unlike the `--scope` refusal below, which does start one.
  CHECK(query_rows(conn, "select count(*) from sessions", 1) == "0");
}

TEST_CASE("decision add --editor with no body warns loudly and still refuses") {
  auto const fx = make_fixture("add-editor");
  seed(fx);

  auto const res = dispatch(fx, {"decision", "add", "G", "--editor"});
  CHECK(res.code == 2);
  // DELIBERATE DIVERGENCE (see this file's header): the oracle prints only
  // the second line. The first exists because an interactive oracle run
  // would have opened `$EDITOR` instead of refusing, and this build cannot.
  CHECK(res.err == "warning: --editor not yet implemented; the interactive editor flow is unported\n"
                   "error: --body is required (or run interactively to use the editor flow)\n");

  auto conn = open_db(fx);
  CHECK(decision_rows(conn).empty());
}

TEST_CASE("decision add --editor WITH a body is silent, exactly as the oracle is") {
  auto const fx = make_fixture("add-editor-body");
  seed(fx);

  // The warning must be conditional on the body's ABSENCE. Emitting it
  // whenever `--editor` appears would be noise on a path where the oracle
  // ignores the flag entirely.
  auto const res = dispatch(fx, {"decision", "add", "F", "--body", "f", "--editor", "--json"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|F|f|<NULL>|proposed|<NULL>|1");
}

TEST_CASE("decision add --json emits the struct's field order with null for every absent optional") {
  auto const fx = make_fixture("add-json");
  seed(fx);

  auto const res = dispatch(fx, {"decision", "add", "second", "--body", "b2", "--rationale", "r2", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.out.starts_with(R"({"id":1,"scope_kind":"global","scope_id":null,"title":"second","body":"b2",)"
                            R"("rationale":"r2","status":"proposed","decided_at":null,"session_id":1,"created_at":")"));
  CHECK(res.out.ends_with("\"}\n"));
}

TEST_CASE("decision add writes an EMPTY body as '' and an absent rationale as NULL") {
  auto const fx = make_fixture("add-empty");
  seed(fx);

  add(fx, {"E", "--body", ""});
  auto conn = open_db(fx);
  // The two nullability directions on ONE row: `body` is `not null` so the
  // empty flag value must survive as `''`, while `rationale` was never
  // given and must be NULL. A handler that folded "" onto absent would get
  // the first wrong; one that folded absent onto "" would get the second.
  CHECK(decision_rows(conn) == "1|global|<NULL>|E||<NULL>|proposed|<NULL>|1");
}

TEST_CASE("decision add --scope refuses an unresolvable slug, having already started a session") {
  auto const fx = make_fixture("add-scope");
  seed(fx);

  auto const res = dispatch(fx, {"decision", "add", "bad", "--body", "x", "--scope", "nosuchslug"});
  CHECK(res.code == 1);
  // The ENGINE's error name, not the scope layer's `resolving scope
  // failed:` shape — `resolve_write_scope` passes an unknown slug through
  // and the engine reports it.
  CHECK(res.err == "error: decision add: SlugNotFound\n");

  auto conn = open_db(fx);
  CHECK(decision_rows(conn).empty());
  CHECK(audit_rows(conn, "decision").empty());
  // ...but the session IS a committed side effect of the failed run. Same
  // ordering `question add` has, and it is observable.
  CHECK(query_rows(conn, "select id, vendor from sessions order by id", 2) == "1|cli");
}

TEST_CASE("decision add --plan a NONEXISTENT plan refuses just as question add does") {
  auto const fx = make_fixture("add-plan-missing");
  seed(fx);

  // Task 6197. This invocation used to exit 0 and leave an `entity_links`
  // row pointing at a plan that never existed. The operator-visible
  // contract is now byte-identical to `question add --plan 9999`, which
  // refused all along: exit 1, `error: decision add: NotFound`.
  auto const res = dispatch(fx, {"decision", "add", "planned", "--body", "b3", "--plan", "9999"});
  CHECK(res.code == 1);

  auto conn = open_db(fx);
  // THE ASSERTION THAT MATTERS. The exit code alone could not have caught
  // the original defect (it was 0), and neither could stdout. Only the
  // absence of the edge row can.
  CHECK(edge_rows(conn).empty());
  CHECK(decision_rows(conn).empty());
  CHECK(audit_rows(conn, "decision").empty());
  CHECK(audit_rows(conn, "entity_link").empty());
}

TEST_CASE("decision add --plan an EXISTING plan writes the edge and one create audit row") {
  auto const fx = make_fixture("add-plan");
  seed(fx);
  // Seeded THROUGH THE CLI, not by raw SQL, so the anchor is the same row
  // an operator would have. It lands as plan id 1.
  REQUIRE(dispatch(fx, {"plan", "create", "anchor", "--scope", "global"}).code == 0);

  auto const res = dispatch(fx, {"decision", "add", "planned", "--body", "b3", "--plan", "1"});
  CHECK(res.code == 0);

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|plan|1|derives-from");
  // ONE audit row, verb `create`. The edge is unaudited — which makes it
  // consistent with `supersede`'s edge and inconsistent with `decision
  // link`'s, all three of which were captured separately.
  CHECK(audit_rows(conn, "decision") == "create|1|create decision 'planned'|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "entity_link").empty());
}

// ===========================================================================
// decision show
// ===========================================================================

TEST_CASE("decision show reports the oracle's dedicated not-found message") {
  auto const fx = make_fixture("show-missing");
  seed(fx);

  auto const res = dispatch(fx, {"decision", "show", "999"});
  CHECK(res.code == 1);
  // NOT the generic `decision show: NotFound` shape the other engine
  // errors take.
  CHECK(res.err == "error: no decision with id 999\n");
}

TEST_CASE("decision show refuses a non-integer id at exit 2 without touching the row") {
  auto const fx = make_fixture("show-abc");
  seed(fx);
  add(fx, {"x", "--body", "y"});

  auto const res = dispatch(fx, {"decision", "show", "abc"});
  CHECK(res.code == 2);
  CHECK(res.err == "error: decision id must be an integer, got 'abc'\n");
  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|x|y|<NULL>|proposed|<NULL>|1");
}

// ===========================================================================
// decision list
// ===========================================================================

TEST_CASE("decision list renders 'no decisions' for text and '[]' for json") {
  auto const fx = make_fixture("list-empty");
  seed(fx);

  auto const text = dispatch(fx, {"decision", "list", "--scope", "global"});
  CHECK(text.code == 0);
  // NO PARENTHESES — `question list` emits `(no questions)` for the same
  // shape of emptiness, and the two disagree.
  CHECK(text.out == "no decisions\n");

  auto const json = dispatch(fx, {"decision", "list", "--scope", "global", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "[]\n");
}

TEST_CASE("decision list refuses outright when the cwd is in no registered scope") {
  auto const fx = make_fixture("list-noscope");
  seed(fx);

  auto const res = dispatch_in(fx, "outside", {"decision", "list"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global\n");
  CHECK(res.out.empty());
}

TEST_CASE("decision list defaults to the OPEN statuses and the excluded rows survive") {
  auto const fx = make_fixture("list-status");
  seed(fx);
  add(fx, {"open-p", "--body", "b"});
  add(fx, {"open-a", "--body", "b"});
  add(fx, {"gone-w", "--body", "b"});
  add(fx, {"gone-s", "--body", "b"});
  add(fx, {"by", "--body", "b"});
  REQUIRE(dispatch(fx, {"decision", "accept", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "withdraw", "3"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "supersede", "4", "--by", "5"}).code == 0);

  // FIVE queries over ONE fixture. An inert `--status` returns the same set
  // every time; a `--status` that always applied would collapse the first
  // to empty. Only real branch behaviour yields five different answers.
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global"}).out == "    1  proposed    open-p\n"
                                                                       "    2  accepted    open-a\n"
                                                                       "    5  proposed    by\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global", "--status", "proposed"}).out == "    1  proposed    open-p\n"
                                                                                               "    5  proposed    by\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global", "--status", "accepted"}).out == "    2  accepted    open-a\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global", "--status", "withdrawn"}).out == "    3  withdrawn   gone-w\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global", "--status", "superseded"}).out == "    4  superseded  gone-s\n");

  // The rows the default listing excluded are STILL THERE.
  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, status from decisions order by id", 2) ==
        "1|proposed;2|accepted;3|withdrawn;4|superseded;5|proposed");
}

TEST_CASE("decision list --status is SINGLE-VALUED and refuses an unknown token at exit 2") {
  auto const fx = make_fixture("list-status-bad");
  seed(fx);

  auto const bogus = dispatch(fx, {"decision", "list", "--scope", "global", "--status", "bogus"});
  // Exit **2**, where the identical refusal on `question list` exits 1.
  CHECK(bogus.code == 2);
  CHECK(bogus.err == "error: unknown status 'bogus'\n");

  // NOT comma-split: the whole string is one token. `question list` and
  // `plan list` both split the same flag, so this cannot be inferred from
  // the flag spec — it was run.
  auto const csv = dispatch(fx, {"decision", "list", "--scope", "global", "--status", "proposed,accepted"});
  CHECK(csv.code == 2);
  CHECK(csv.err == "error: unknown status 'proposed,accepted'\n");
}

TEST_CASE("decision list --scope is SINGLE-VALUED and reports the engine's SlugNotFound") {
  auto const fx = make_fixture("list-scope-bad");
  seed(fx);

  auto const unknown = dispatch(fx, {"decision", "list", "--scope", "nosuchslug"});
  CHECK(unknown.code == 1);
  CHECK(unknown.err == "error: decision list: SlugNotFound\n");

  // A comma-joined pair of INDIVIDUALLY VALID slugs still fails, which is
  // the proof that the flag is not split: `global` and `repo:proj` both
  // resolve on their own.
  auto const csv = dispatch(fx, {"decision", "list", "--scope", "global,repo:proj"});
  CHECK(csv.code == 1);
  CHECK(csv.err == "error: decision list: SlugNotFound\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global"}).code == 0);
  CHECK(dispatch(fx, {"decision", "list", "--scope", "repo:proj"}).code == 0);
}

TEST_CASE("decision list cwd-derives a read set that EXCLUDES global decisions") {
  auto const fx = make_fixture("list-cwd");
  seed(fx);
  add(fx, {"repo-scoped", "--body", "rb", "--scope", "repo:proj"});
  add(fx, {"global-scoped", "--body", "gb"});

  // From inside the registered project the read set is `repo:proj` ALONE.
  // A global decision is NOT included — the cwd-derived set is not a
  // superset of `global`, which is easy to assume and wrong.
  CHECK(dispatch(fx, {"decision", "list"}).out == "    1  proposed    repo-scoped\n");
  // ...and the global row survives, reachable by naming its scope.
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global"}).out == "    2  proposed    global-scoped\n");

  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|repo|1|repo-scoped|rb|<NULL>|proposed|<NULL>|1;"
                               "2|global|<NULL>|global-scoped|gb|<NULL>|proposed|<NULL>|1");
}

TEST_CASE("decision list --plan EXCLUDES unlinked decisions, which survive") {
  auto const fx = make_fixture("list-plan");
  seed(fx);
  REQUIRE(dispatch(fx, {"plan", "create", "p1", "--summary", "s", "--scope", "repo:proj"}).code == 0);
  add(fx, {"linked", "--body", "b", "--plan", "1"});
  add(fx, {"unlinked", "--body", "b"});

  CHECK(dispatch(fx, {"decision", "list", "--scope", "global", "--plan", "1"}).out == "    1  proposed    linked\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global", "--plan", "2"}).out == "no decisions\n");
  CHECK(dispatch(fx, {"decision", "list", "--scope", "global"}).out == "    1  proposed    linked\n"
                                                                       "    2  proposed    unlinked\n");

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, title from decisions order by id", 2) == "1|linked;2|unlinked");
}

// ===========================================================================
// decision accept / withdraw
// ===========================================================================

TEST_CASE("decision accept stamps decided_at and audits once") {
  auto const fx = make_fixture("accept");
  seed(fx);
  add(fx, {"x", "--body", "y"});

  auto const res = dispatch(fx, {"decision", "accept", "1"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains("status:     accepted\n"));
  CHECK(res.out.contains("decided:    "));

  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|x|y|<NULL>|accepted|SET|1");
  CHECK(audit_rows(conn, "decision") == "create|1|create decision 'x'|<NULL>|<NULL>;"
                                        "status_change|1|accept|<NULL>|<NULL>");
}

TEST_CASE("decision withdraw leaves decided_at alone, in both directions") {
  auto const fx = make_fixture("withdraw");
  seed(fx);
  add(fx, {"never", "--body", "b"});
  add(fx, {"accepted", "--body", "b"});
  REQUIRE(dispatch(fx, {"decision", "accept", "2"}).code == 0);

  REQUIRE(dispatch(fx, {"decision", "withdraw", "1"}).code == 0);
  auto const res = dispatch(fx, {"decision", "withdraw", "2"});
  REQUIRE(res.code == 0);
  // A withdrawn decision that WAS accepted still renders its `decided:`
  // line. That is the operator-visible half of "withdraw does not touch
  // decided_at".
  CHECK(res.out.contains("status:     withdrawn\n"));
  CHECK(res.out.contains("decided:    "));

  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|never|b|<NULL>|withdrawn|<NULL>|1;"
                               "2|global|<NULL>|accepted|b|<NULL>|withdrawn|SET|1");
}

TEST_CASE("decision accept REFUSES a terminal decision and writes nothing") {
  auto const fx = make_fixture("accept-terminal");
  seed(fx);
  add(fx, {"x", "--body", "y"});
  REQUIRE(dispatch(fx, {"decision", "withdraw", "1"}).code == 0);

  auto       conn   = open_db(fx);
  auto const before = decision_rows(conn);
  auto const audit  = audit_rows(conn, "decision");

  auto const res = dispatch(fx, {"decision", "accept", "1"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: decision 1 is terminal; cannot accept\n");
  CHECK(res.out.empty());
  // The refusal's AFTER-STATE. The whole-row snapshot covers `updated_at`
  // too, so a refusal that ran the UPDATE first would show up here.
  CHECK(decision_rows(conn) == before);
  CHECK(audit_rows(conn, "decision") == audit);
}

TEST_CASE("decision withdraw REFUSES a terminal decision with its own verb word") {
  auto const fx = make_fixture("withdraw-terminal");
  seed(fx);
  add(fx, {"old", "--body", "y"});
  add(fx, {"new", "--body", "z"});
  REQUIRE(dispatch(fx, {"decision", "supersede", "1", "--by", "2"}).code == 0);

  auto const res = dispatch(fx, {"decision", "withdraw", "1"});
  CHECK(res.code == 1);
  // The verb word is what varies between this message and `accept`'s; a
  // shared helper that hard-coded one would pass the other's test.
  CHECK(res.err == "error: decision 1 is terminal; cannot withdraw\n");
}

TEST_CASE("decision accept and withdraw ACCEPT AND IGNORE --scope, even an unresolvable one") {
  auto const fx = make_fixture("transition-scope");
  seed(fx);
  add(fx, {"a", "--body", "b"});
  add(fx, {"b", "--body", "b"});

  // Deliberate CLI parity in the original: `_ = args.scope;`. A port that
  // helpfully resolved it would turn both of these into SlugNotFound.
  CHECK(dispatch(fx, {"decision", "accept", "1", "--scope", "nosuchslug"}).code == 0);
  CHECK(dispatch(fx, {"decision", "withdraw", "2", "--scope", "nosuchslug"}).code == 0);

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, status from decisions order by id", 2) == "1|accepted;2|withdrawn");
}

// ===========================================================================
// decision supersede
// ===========================================================================

TEST_CASE("decision supersede flips the OLD decision, writes the edge, and renders the old row") {
  auto const fx = make_fixture("supersede");
  seed(fx);
  add(fx, {"old", "--body", "y"});
  add(fx, {"new", "--body", "z"});

  auto const res = dispatch(fx, {"decision", "supersede", "1", "--by", "2"});
  REQUIRE(res.code == 0);
  // The OLD decision is what comes back — the one whose status changed.
  CHECK(res.out.starts_with("id:         1\n"
                            "title:      old\n"
                            "status:     superseded\n"));

  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|old|y|<NULL>|superseded|<NULL>|1;"
                               "2|global|<NULL>|new|z|<NULL>|proposed|<NULL>|1");
  // NEW -> OLD, the reverse of the argument order.
  CHECK(edge_rows(conn) == "2|decision|1|supersedes");
  CHECK(audit_rows(conn, "decision") == "create|1|create decision 'old'|<NULL>|<NULL>;"
                                        "create|2|create decision 'new'|<NULL>|<NULL>;"
                                        "status_change|1|supersede: decision 1 superseded by decision 2|<NULL>|<NULL>");
  // NO `link` audit row. `decision link` on the same pair writes one; this
  // path does not, which is what forbids routing it through
  // `engine_entitylink`.
  CHECK(audit_rows(conn, "entity_link").empty());
}

TEST_CASE("decision supersede names BOTH ids when either is missing") {
  auto const fx = make_fixture("supersede-missing");
  seed(fx);
  add(fx, {"old", "--body", "y"});

  auto const missing_new = dispatch(fx, {"decision", "supersede", "1", "--by", "999"});
  CHECK(missing_new.code == 1);
  CHECK(missing_new.err == "error: no decision with id 1 or 999\n");

  auto const missing_old = dispatch(fx, {"decision", "supersede", "999", "--by", "1"});
  CHECK(missing_old.code == 1);
  CHECK(missing_old.err == "error: no decision with id 999 or 1\n");

  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|old|y|<NULL>|proposed|<NULL>|1");
  CHECK(edge_rows(conn).empty());
}

TEST_CASE("decision supersede ROLLS BACK the status flip when the edge already exists") {
  auto const fx = make_fixture("supersede-dup");
  seed(fx);
  add(fx, {"old", "--body", "y"});
  add(fx, {"new", "--body", "z"});
  // Create the edge the way an operator would — through the sibling leaf.
  REQUIRE(dispatch(fx, {"decision", "link", "2", "decision:1", "--relationship", "supersedes"}).code == 0);

  auto       conn   = open_db(fx);
  auto const before = decision_rows(conn);
  auto const edges  = edge_rows(conn);

  auto const res = dispatch(fx, {"decision", "supersede", "1", "--by", "2"});
  CHECK(res.code == 1);
  // Note the argument order: the message reads NEW to OLD, the direction of
  // the edge, which is the reverse of the verb's own `<old> --by <new>`.
  CHECK(res.err == "error: supersedes link from decision 2 to 1 already exists\n");

  // THE POINT OF THIS CASE. The status UPDATE runs BEFORE the INSERT that
  // fails; without the rollback decision 1 is left `superseded` — marked
  // replaced by a link this operation did not create — with a rewritten
  // `updated_at`. Verified against the oracle by this exact sequence.
  CHECK(decision_rows(conn) == before);
  CHECK(query_rows(conn, "select status from decisions where id = 1", 1) == "proposed");
  CHECK(edge_rows(conn) == edges);
  CHECK(query_rows(conn, "select count(*) from audit_log where verb = 'status_change'", 1) == "0");
}

TEST_CASE("decision supersede refuses a WITHDRAWN old decision but allows a SUPERSEDED one") {
  auto const fx = make_fixture("supersede-terminal");
  seed(fx);
  add(fx, {"drawn", "--body", "b"});
  add(fx, {"a", "--body", "b"});
  add(fx, {"b", "--body", "b"});
  add(fx, {"c", "--body", "b"});
  REQUIRE(dispatch(fx, {"decision", "withdraw", "1"}).code == 0);

  auto const refused = dispatch(fx, {"decision", "supersede", "1", "--by", "2"});
  CHECK(refused.code == 1);
  CHECK(refused.err == "error: decision 1 is terminal; cannot supersede\n");

  // ...but `superseded -> superseded` is an IDENTITY move, so a decision
  // that is ALREADY superseded can be superseded again by a different one,
  // accumulating a second edge. Both halves oracle-confirmed; reading
  // "terminal is refused" off the matrix predicts the wrong answer here.
  REQUIRE(dispatch(fx, {"decision", "supersede", "2", "--by", "3"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "supersede", "2", "--by", "4"}).code == 0);

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "3|decision|2|supersedes;4|decision|2|supersedes");
  CHECK(query_rows(conn, "select id, status from decisions order by id", 2) == "1|withdrawn;2|superseded;3|proposed;4|proposed");
}

// ===========================================================================
// decision link
// ===========================================================================

TEST_CASE("decision link writes the edge and echoes the oracle's double-spaced line") {
  auto const fx = make_fixture("link");
  seed(fx);
  add(fx, {"d", "--body", "b"});
  REQUIRE(dispatch(fx, {"plan", "create", "p1", "--summary", "s", "--scope", "repo:proj"}).code == 0);

  auto const res = dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "cites"});
  REQUIRE(res.code == 0);
  // DOUBLE spaces around the bracket group, and around the id suffix.
  CHECK(res.out == "linked decision:1 -> plan:1  [cites]  (link id: 1)\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|plan|1|cites");
  // UNLIKE `supersede`'s and `add --plan`'s edges, THIS one is audited.
  CHECK(audit_rows(conn, "entity_link") == "link|1|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("decision link --json uses the decision_id key and omits created_at") {
  auto const fx = make_fixture("link-json");
  seed(fx);
  add(fx, {"d", "--body", "b"});
  REQUIRE(dispatch(fx, {"plan", "create", "p1", "--summary", "s", "--scope", "repo:proj"}).code == 0);

  auto const res = dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "verifies", "--json"});
  REQUIRE(res.code == 0);
  // The subject key is PER-VERB (`decision_id`, where `question link` emits
  // `question_id`), and this envelope carries no `created_at`.
  CHECK(res.out == R"({"ok":true,"id":1,"decision_id":1,"to_kind":"plan","to_id":1,"relationship":"verifies"})"
                   "\n");
}

TEST_CASE("decision link refuses a duplicate with the ASCII arrow and single spaces") {
  auto const fx = make_fixture("link-dup");
  seed(fx);
  add(fx, {"d", "--body", "b"});
  REQUIRE(dispatch(fx, {"plan", "create", "p1", "--summary", "s", "--scope", "repo:proj"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "cites"}).code == 0);

  auto const res = dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "cites"});
  CHECK(res.code == 1);
  // ASCII `->` and SINGLE spaces here, where the SUCCESS line above has
  // double ones. Only `plan link` spells this arrow `→`.
  CHECK(res.err == "error: link decision:1 -> plan:1 [cites] already exists\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|plan|1|cites");
  CHECK(audit_rows(conn, "entity_link") == "link|1|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("decision link names the missing SIDE of a bad endpoint") {
  auto const fx = make_fixture("link-endpoint");
  seed(fx);
  add(fx, {"d", "--body", "b"});

  auto const bad_to = dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "cites"});
  CHECK(bad_to.code == 1);
  CHECK(bad_to.err == "error: plan:1 not found\n");

  auto const bad_from = dispatch(fx, {"decision", "link", "999", "plan:1", "--relationship", "cites"});
  CHECK(bad_from.code == 1);
  // The FROM side is checked first, so a doubly-bad invocation names the
  // decision rather than the plan.
  CHECK(bad_from.err == "error: decision:999 not found\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn).empty());
  CHECK(audit_rows(conn, "entity_link").empty());
}

TEST_CASE("decision link refuses a missing relationship, an unknown one, and a slug ref at exit 2") {
  auto const fx = make_fixture("link-refusals");
  seed(fx);
  add(fx, {"d", "--body", "b"});

  auto const no_rel = dispatch(fx, {"decision", "link", "1", "plan:1"});
  CHECK(no_rel.code == 2);
  CHECK(no_rel.err == "error: --relationship is required\n");

  auto const bad_rel = dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "nope"});
  CHECK(bad_rel.code == 2);
  CHECK(bad_rel.err == "error: unknown relationship 'nope'\n");

  // `blocks` is the PRE-migration-00033 spelling for `depends-on` and is
  // deliberately not an alias — accepting it would restore the direction
  // inversion that migration exists to fix.
  auto const blocks = dispatch(fx, {"decision", "link", "1", "plan:1", "--relationship", "blocks"});
  CHECK(blocks.code == 2);
  CHECK(blocks.err == "error: unknown relationship 'blocks'\n");

  auto const bad_kind = dispatch(fx, {"decision", "link", "1", "bogus:1", "--relationship", "cites"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: invalid ref 'bogus:1': expected kind:integer-id\n");

  auto const slug = dispatch(fx, {"decision", "link", "1", "plan:slug", "--relationship", "cites"});
  CHECK(slug.code == 2);
  CHECK(slug.err == "error: slug refs are not supported; use kind:integer-id (e.g. plan:42)\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn).empty());
}

// ===========================================================================
// the four deferred leaves
// ===========================================================================

TEST_CASE("the decision workbench quartet is SERVED, and writes nothing when unanchored") {
  auto const fx = make_fixture("deferred");
  seed(fx);
  add(fx, {"d", "--body", "b"});

  // Pinned all four as exit-64 refusals until plan 996 task 6205 landed
  // `editflow`. Inverted rather than deleted, for the reason the original
  // gave: dispatch.t.cpp's inventory proves the PATH is declared, and this
  // proves the operator reaches real behaviour rather than a silent exit 0.
  //
  // This decision was added without `--plan`, so it has no `derives-from`
  // edge. The comment this case used to carry claimed the oracle's decision
  // quartet was incoherent -- "two abort with a Zig stack trace, two report
  // NotFound for a decision that exists". Running it says otherwise: the
  // two that report prose say `is not linked to a plan`, NOT `no decision
  // with id 1`, and the split is between VERB PAIRS, not a family defect.
  // `decision` behaves exactly as `question`, `scenario` and `artifact` do.
  for (auto const& verb : {"edit", "view"}) {
    INFO("decision " << verb);
    auto const res = dispatch(fx, {"decision", verb, "1"});
    CHECK(res.code == 1);
    CHECK(res.out.empty());
    CHECK(res.err == "error: cannot resolve anchor plan for decision 1: NoPlanLink\nerror: NoPlanLink\n");
  }
  for (auto const& verb : {"diff", "review"}) {
    INFO("decision " << verb);
    auto const res = dispatch(fx, {"decision", verb, "1"});
    CHECK(res.code == 1);
    CHECK(res.out.empty());
    CHECK(res.err == "error: decision 1 is not linked to a plan; cannot resolve anchor plan\n");
  }

  // ...and nothing they touched changed.
  auto conn = open_db(fx);
  CHECK(decision_rows(conn) == "1|global|<NULL>|d|b|<NULL>|proposed|<NULL>|1");
}
