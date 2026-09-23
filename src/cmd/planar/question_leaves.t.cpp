// @file question_leaves.t.cpp
// @brief In-process tests for the six leaves wired by plan 996, task 6188:
// the five `question` ones (`add`, `show`, `list`, `answer`, `wontfix`) and
// `assoc members`, which the same cycle picked up because its only missing
// piece was a renderer. The `assoc members` block is at the bottom, under
// its own banner and its own oracle captures; it lives here rather than in
// a file of its own because it is ONE leaf.
//
// Its own file rather than more of `handlers.t.cpp` (3.8k lines already),
// on the discipline `annotate_leaves.t.cpp` and
// `plan_task_remainder_leaves.t.cpp` established.
//
// ## EVERY CASE ASSERTS DATABASE ROWS
//
// Stdout is checked where the bytes are the contract, but no case rests on
// stdout alone. The defect class this milestone keeps finding is a verb
// that exits 0 with oracle-identical output and wrong rows. So every
// mutation here is followed by a `question_rows` / `audit_rows` snapshot,
// and every REFUSAL is followed by one too — a refusal that half-wrote is
// the worse bug, and only the after-state distinguishes it.
//
// SQL NULL renders as the literal `<NULL>` and is never collapsed onto the
// empty string. `questions.body` is genuinely nullable and an absent
// `--body` must produce NULL, not `""`: the two render as `null` and `""`
// in `--json`, so an engine that folded one onto the other would be
// operator-visible and would still pass every title-based assertion.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, BY A SURVIVOR
//
// Each `question list` case seeds rows on BOTH sides of its predicate and
// asserts the returned set AND that the excluded rows are still in the
// table afterwards.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string was captured from `zig/zig-out/bin/planar`
// against a scratch `PLANAR_DB`, read back through a Python `repr` rather
// than through a pipe into `tail`. The whole set was then re-derived as a
// SEQUENCE diff: a 44-step argv script replayed against both binaries with
// their stdout, stderr, exit codes AND resulting `questions` /
// `audit_log` / `entity_links` / `sessions` / `agent_actions` dumps
// compared, plus a second 10-step script for the `--touches` UNION. The
// captures that decided a shape:
//
//   $Z question list                 exit 0  stdout b'(no questions)\n'
//   $Z question list --json          exit 0  stdout b'[]\n'
//       ^ the empty case is a WORD, not zero bytes, and the two forms
//         disagree about which word.
//   $Z question add "q"              stdout b'id:        1\ntitle:     q\n...'
//       ^ values start at column 12. `plan`'s block starts at 11.
//   $Z question answer 1 --answer x  twice   BOTH exit 0
//       ^ answered -> answered is the identity short-circuit, not a
//         refusal. The second call OVERWRITES answer_body.
//   $Z question wontfix <answered>   exit 1  b'error: question wontfix: IllegalTransition\n'
//   $Z question answer 3             exit 2  b'error: --answer is required\n'
//   $Z question answer 3 --answer "" exit 1  b'error: --answer must be non-empty\n'
//       ^ ONE flag, TWO refusals, TWO different exit codes.
//   $Z question show 999             exit 1  b'error: no question with id 999\n'
//       ^ NOT the generic `question show: NotFound` shape.
//   $Z question add "x" --scope nope exit 1  b'error: question add: SlugNotFound\n'
//       AND a `sessions` row plus a `create|session|1|start session
//       vendor=cli` audit row, written BEFORE the create failed.
//   $Z question add "x" --editor     exit 0, row created, stderr
//       b'warning: --editor not yet implemented; falling back to inline create\n'
//   $Z question list --touches nope  exit 1  b"error: repo 'nope' not found\n"
//       ^ an unknown repo REFUSES; it does not list empty.
//
#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "json_envelope_test_support.hpp"

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
                         std::format("planar_q_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", std::make_shared<planar::cmd::database>(fx.db_path, err), out, err};
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

/// @brief Every `questions` row as
/// `id|scope_kind|scope_id|title|body|status|answer_body|answered_at`.
///
/// `answered_at` is a wall-clock timestamp, so it is projected as the
/// three-state `<NULL>` / `SET` rather than verbatim — the assertion that
/// matters is whether the column is NULL, which the schema's row CHECK ties
/// to `status = 'answered'`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto question_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select id, scope_kind, scope_id, title, body, status, answer_body, "
                    "case when answered_at is null then null else 'SET' end from questions order by id",
                    8);
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

/// @brief Every `question -> *` edge as `from_id|to_kind|to_id|relationship`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto edge_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select from_id, to_kind, to_id, relationship from entity_links where from_kind = 'question' order by id", 4);
}

/// @brief Bring a fixture up to an initialised database.
/// @param fx The fixture.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
}

} // namespace

// ===========================================================================
// question add
// ===========================================================================

TEST_CASE("question add writes an open row and echoes the oracle's block") {
  auto const fx = make_fixture("add");
  seed(fx);

  auto const added = dispatch(fx, {"question", "add", "first question", "--body", "some body", "--scope", "global"});
  CHECK(added.code == 0);
  CHECK(added.err.empty());
  // Values start at column 12; `body:` sits between `scope:` and
  // `created:`. Timestamps vary, so the two trailing lines are matched by
  // prefix rather than pinned.
  CHECK(added.out.starts_with("id:        1\n"
                              "title:     first question\n"
                              "status:    open\n"
                              "scope:     global\n"
                              "body:      some body\n"
                              "created:   "));
  CHECK(added.out.contains("\nupdated:   "));

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|first question|some body|open|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "question") == "create|1|create question 'first question'|<NULL>|<NULL>");
  CHECK(edge_rows(conn).empty());
}

TEST_CASE("question add with NO --body writes SQL NULL, and --json renders it as null") {
  auto const fx = make_fixture("addnull");
  seed(fx);

  REQUIRE(dispatch(fx, {"question", "add", "no body", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "empty body", "--scope", "global", "--body", ""}).code == 0);

  {
    auto conn = open_db(fx);
    // The discrimination: row 1 is NULL, row 2 is the empty string. An
    // engine that bound "" for an absent optional collapses these and
    // still produces identical `question list` output.
    CHECK(question_rows(conn) == "1|global|<NULL>|no body|<NULL>|open|<NULL>|<NULL>;"
                                 "2|global|<NULL>|empty body||open|<NULL>|<NULL>");
  }

  // ...and the same distinction survives into the rendered JSON, which is
  // where a read-side collapse (invisible to the row snapshot above) would
  // show up.
  auto const null_json = dispatch(fx, {"question", "show", "1", "--json"});
  CHECK(null_json.code == 0);
  CHECK(null_json.out.contains(R"("body":null)"));
  auto const empty_json = dispatch(fx, {"question", "show", "2", "--json"});
  CHECK(empty_json.code == 0);
  CHECK(empty_json.out.contains(R"("body":"")"));
}

TEST_CASE("question add --scope resolves each grammar and an unknown slug REFUSES") {
  auto const fx = make_fixture("addscope");
  seed(fx);
  REQUIRE(dispatch(fx, {"assoc", "create", "acme", "--kind", "org", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"question", "add", "global q", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "assoc q", "--scope", "acme"}).code == 0);

  auto const bad = dispatch(fx, {"question", "add", "bad q", "--scope", "nope"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: question add: SlugNotFound\n");

  auto conn = open_db(fx);
  // The refusal wrote no third row and no third audit row.
  CHECK(question_rows(conn) == "1|global|<NULL>|global q|<NULL>|open|<NULL>|<NULL>;"
                               "2|association|1|assoc q|<NULL>|open|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "question") == "create|1|create question 'global q'|<NULL>|<NULL>;"
                                        "create|2|create question 'assoc q'|<NULL>|<NULL>");
}

TEST_CASE("question add --plan links the question; a MISSING plan refuses without writing") {
  auto const fx = make_fixture("addplan");
  seed(fx);
  REQUIRE(dispatch(fx, {"plan", "create", "P", "--scope", "global", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"question", "add", "linked", "--scope", "global", "--plan", "1"}).code == 0);

  auto const dangling = dispatch(fx, {"question", "add", "dangling", "--scope", "global", "--plan", "999"});
  CHECK(dangling.code == 1);
  CHECK(dangling.err == "error: question add: NotFound\n");

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|linked|<NULL>|open|<NULL>|<NULL>");
  CHECK(edge_rows(conn) == "1|plan|1|derives-from");
  // ONE audit row for the create; the edge does NOT get a `link` row.
  CHECK(audit_rows(conn, "question") == "create|1|create question 'linked'|<NULL>|<NULL>");
}

TEST_CASE("question add --editor warns on stderr and creates the row anyway") {
  auto const fx = make_fixture("addeditor");
  seed(fx);

  auto const added = dispatch(fx, {"question", "add", "editor q", "--scope", "global", "--editor"});
  CHECK(added.code == 0);
  // A WARNING, not a refusal — the create proceeds. Captured verbatim.
  CHECK(added.err == "warning: --editor not yet implemented; falling back to inline create\n");
  CHECK(added.out.starts_with("id:        1\n"));

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|editor q|<NULL>|open|<NULL>|<NULL>");
}

TEST_CASE("question add opens a session BEFORE the create, even when the create then fails") {
  auto const fx = make_fixture("addsession");
  seed(fx);

  auto const bad = dispatch(fx, {"question", "add", "x", "--scope", "nope"});
  REQUIRE(bad.code == 1);

  auto conn = open_db(fx);
  // The ordering is observable and question-specific: `plan create` and
  // `task add` open no session at all. A port that moved the session onto
  // the success path would leave no row here and pass every stdout check.
  CHECK(query_rows(conn, "select count(*), vendor from sessions", 2) == "1|cli");
  CHECK(question_rows(conn).empty());

  // Session creation succeeds before the subsequent scope refusal, so its
  // audit row remains even though the question row does not.
  CHECK(audit_rows(conn, "session") == "create|1|start session vendor=cli|<NULL>|<NULL>");
}

// ===========================================================================
// question show
// ===========================================================================

TEST_CASE("question show renders text and JSON, and NotFound has its OWN message") {
  auto const fx = make_fixture("show");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "q one", "--scope", "global"}).code == 0);

  auto const text = dispatch(fx, {"question", "show", "1"});
  CHECK(text.code == 0);
  CHECK(text.out.starts_with("id:        1\ntitle:     q one\nstatus:    open\nscope:     global\ncreated:   "));

  auto const json = dispatch(fx, {"question", "show", "1", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.starts_with(R"({"id":1,"scope_kind":"global","scope_id":null,"title":"q one","body":null,)"
                             R"("status":"open","answer_body":null,"answered_at":null,"created_at":")"));
  CHECK(json.out.ends_with("\"}\n"));

  // NOT `question show: NotFound` — this arm has its own wording.
  auto const missing = dispatch(fx, {"question", "show", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no question with id 999\n");

  // A non-integer id is exit 2 with the shared helper's wording.
  auto const bad = dispatch(fx, {"question", "show", "abc"});
  CHECK(bad.code == 2);
  CHECK(bad.err == "error: question id must be an integer, got 'abc'\n");

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|q one|<NULL>|open|<NULL>|<NULL>");
}

// ===========================================================================
// question answer / wontfix
// ===========================================================================

TEST_CASE("question answer writes all three columns and one status_change row") {
  auto const fx = make_fixture("answer");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "do or do not", "--scope", "global"}).code == 0);

  auto const answered = dispatch(fx, {"question", "answer", "1", "--answer", "do"});
  CHECK(answered.code == 0);
  CHECK(answered.out.contains("status:    answered\n"));
  CHECK(answered.out.contains("answer:    do\n"));
  CHECK(answered.out.contains("\nanswered:  "));

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|do or do not|<NULL>|answered|do|SET");
  CHECK(audit_rows(conn, "question") == "create|1|create question 'do or do not'|<NULL>|<NULL>;"
                                        "status_change|1|answer: do|<NULL>|<NULL>");
}

TEST_CASE("question answer: absent flag exits 2, EMPTY flag exits 1, neither writes") {
  auto const fx = make_fixture("answerflag");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "q", "--scope", "global"}).code == 0);

  // ONE flag, TWO refusals, TWO exit codes. Collapsing them would be tidier
  // and would diverge.
  auto const absent = dispatch(fx, {"question", "answer", "1"});
  CHECK(absent.code == 2);
  CHECK(absent.err == "error: --answer is required\n");

  auto const empty = dispatch(fx, {"question", "answer", "1", "--answer", ""});
  CHECK(empty.code == 1);
  CHECK(empty.err == "error: --answer must be non-empty\n");

  auto conn = open_db(fx);
  // Still open, and no status_change row from either refusal.
  CHECK(question_rows(conn) == "1|global|<NULL>|q|<NULL>|open|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "question") == "create|1|create question 'q'|<NULL>|<NULL>");
}

TEST_CASE("question answer on a MISSING id refuses; the absent-flag check fires FIRST") {
  auto const fx = make_fixture("answermissing");
  seed(fx);

  // No `--answer` and no such row: the FLAG refusal wins, at exit 2.
  auto const flag_first = dispatch(fx, {"question", "answer", "999"});
  CHECK(flag_first.code == 2);
  CHECK(flag_first.err == "error: --answer is required\n");

  auto const missing = dispatch(fx, {"question", "answer", "999", "--answer", "x"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no question with id 999\n");

  auto conn = open_db(fx);
  CHECK(question_rows(conn).empty());
  CHECK(audit_rows(conn, "question").empty());
}

TEST_CASE("re-answering an answered question SUCCEEDS and overwrites the answer") {
  auto const fx = make_fixture("reanswer");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "q", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "answer", "1", "--answer", "first"}).code == 0);

  // The identity short-circuit, oracle-confirmed by running it twice. Not a
  // no-op either: the row changes.
  auto const second = dispatch(fx, {"question", "answer", "1", "--answer", "second"});
  CHECK(second.code == 0);
  CHECK(second.err.empty());

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|q|<NULL>|answered|second|SET");
  CHECK(audit_rows(conn, "question") == "create|1|create question 'q'|<NULL>|<NULL>;"
                                        "status_change|1|answer: first|<NULL>|<NULL>;"
                                        "status_change|1|answer: second|<NULL>|<NULL>");
}

TEST_CASE("question wontfix records the reason in the AUDIT row only") {
  auto const fx = make_fixture("wontfix");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "with reason", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "without reason", "--scope", "global"}).code == 0);

  CHECK(dispatch(fx, {"question", "wontfix", "1", "--reason", "obsolete"}).code == 0);
  CHECK(dispatch(fx, {"question", "wontfix", "2"}).code == 0);

  auto conn = open_db(fx);
  // Neither row gained an answer_body — `wontfix` must not borrow the
  // answer columns — and `questions` has no reason column at all.
  CHECK(question_rows(conn) == "1|global|<NULL>|with reason|<NULL>|wontfix|<NULL>|<NULL>;"
                               "2|global|<NULL>|without reason|<NULL>|wontfix|<NULL>|<NULL>");
  // `wontfix: <reason>` with one, the BARE WORD without — not `wontfix: `.
  CHECK(audit_rows(conn, "question") == "create|1|create question 'with reason'|<NULL>|<NULL>;"
                                        "create|2|create question 'without reason'|<NULL>|<NULL>;"
                                        "status_change|1|wontfix: obsolete|<NULL>|<NULL>;"
                                        "status_change|2|wontfix|<NULL>|<NULL>");
}

TEST_CASE("both terminals refuse every cross edge, and the refusal writes nothing") {
  auto const fx = make_fixture("terminal");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "answered one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "wontfix one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "answer", "1", "--answer", "yes"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "wontfix", "2"}).code == 0);

  auto const wf_from_answered = dispatch(fx, {"question", "wontfix", "1"});
  CHECK(wf_from_answered.code == 1);
  CHECK(wf_from_answered.err == "error: question wontfix: IllegalTransition\n");

  auto const ans_from_wontfix = dispatch(fx, {"question", "answer", "2", "--answer", "too late"});
  CHECK(ans_from_wontfix.code == 1);
  CHECK(ans_from_wontfix.err == "error: question answer: IllegalTransition\n");

  auto conn = open_db(fx);
  // Unchanged, and in particular row 2 did NOT pick up `too late`.
  CHECK(question_rows(conn) == "1|global|<NULL>|answered one|<NULL>|answered|yes|SET;"
                               "2|global|<NULL>|wontfix one|<NULL>|wontfix|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "question") == "create|1|create question 'answered one'|<NULL>|<NULL>;"
                                        "create|2|create question 'wontfix one'|<NULL>|<NULL>;"
                                        "status_change|1|answer: yes|<NULL>|<NULL>;"
                                        "status_change|2|wontfix|<NULL>|<NULL>");
}

TEST_CASE("re-wontfixing a wontfix question succeeds by the identity short-circuit") {
  auto const fx = make_fixture("rewontfix");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "q", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "wontfix", "1", "--reason", "one"}).code == 0);

  auto const again = dispatch(fx, {"question", "wontfix", "1", "--reason", "two"});
  CHECK(again.code == 0);

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|q|<NULL>|wontfix|<NULL>|<NULL>");
  // Both reasons landed: the second call is a real write, not a no-op.
  CHECK(audit_rows(conn, "question") == "create|1|create question 'q'|<NULL>|<NULL>;"
                                        "status_change|1|wontfix: one|<NULL>|<NULL>;"
                                        "status_change|1|wontfix: two|<NULL>|<NULL>");
}

// ===========================================================================
// question list
// ===========================================================================

TEST_CASE("question list's empty case is a WORD, and the two forms disagree") {
  auto const fx = make_fixture("listempty");
  seed(fx);

  auto const text = dispatch(fx, {"question", "list", "--scope", "global"});
  CHECK(text.code == 0);
  CHECK(text.out == "(no questions)\n");

  auto const json = dispatch(fx, {"question", "list", "--scope", "global", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "[]\n");
}

TEST_CASE("question list's default status is `open`, and the terminal rows SURVIVE") {
  auto const fx = make_fixture("liststatus");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "open one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "answered one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "wontfix one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "answer", "2", "--answer", "yes"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "wontfix", "3"}).code == 0);

  // Reading "no --status" as "no predicate" would list all three.
  auto const defaulted = dispatch(fx, {"question", "list", "--scope", "global"});
  CHECK(defaulted.code == 0);
  CHECK(defaulted.out == "    1  open        open one\n");

  // `--status` is COMMA-SEPARATED on this verb, and spaces are trimmed.
  auto const two = dispatch(fx, {"question", "list", "--scope", "global", "--status", "answered , wontfix"});
  CHECK(two.code == 0);
  CHECK(two.out == "    2  answered    answered one\n"
                   "    3  wontfix     wontfix one\n");

  // An unknown status REFUSES rather than listing empty. Exit 1, not 2.
  auto const bad = dispatch(fx, {"question", "list", "--scope", "global", "--status", "bogus"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: unknown status 'bogus'\n");

  auto conn = open_db(fx);
  // Every excluded row is still in the table: the listing filtered, it did
  // not mutate.
  CHECK(question_rows(conn) == "1|global|<NULL>|open one|<NULL>|open|<NULL>|<NULL>;"
                               "2|global|<NULL>|answered one|<NULL>|answered|yes|SET;"
                               "3|global|<NULL>|wontfix one|<NULL>|wontfix|<NULL>|<NULL>");
}

TEST_CASE("question list --scope EXCLUDES other scopes, and they survive") {
  auto const fx = make_fixture("listscope");
  seed(fx);
  REQUIRE(dispatch(fx, {"assoc", "create", "acme", "--kind", "org", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "global q", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "assoc q", "--scope", "acme"}).code == 0);

  auto const only_global = dispatch(fx, {"question", "list", "--scope", "global"});
  CHECK(only_global.out == "    1  open        global q\n");
  auto const only_assoc = dispatch(fx, {"question", "list", "--scope", "acme"});
  CHECK(only_assoc.out == "    2  open        assoc q\n");
  // Comma-separated and trimmed, OR-ed into one disjunction.
  auto const both = dispatch(fx, {"question", "list", "--scope", " global , acme "});
  CHECK(both.out == "    1  open        global q\n"
                    "    2  open        assoc q\n");

  // An unknown member fails the WHOLE call rather than being dropped — a
  // read verb that silently shortens its scope set returns a plausible,
  // wrong list.
  auto const bad = dispatch(fx, {"question", "list", "--scope", "global,nope"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: question list: SlugNotFound\n");

  auto conn = open_db(fx);
  CHECK(question_rows(conn) == "1|global|<NULL>|global q|<NULL>|open|<NULL>|<NULL>;"
                               "2|association|1|assoc q|<NULL>|open|<NULL>|<NULL>");
}

TEST_CASE("question list --plan EXCLUDES unlinked questions and the wrong plan's") {
  auto const fx = make_fixture("listplan");
  seed(fx);
  REQUIRE(dispatch(fx, {"plan", "create", "P one", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "P two", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "on one", "--scope", "global", "--plan", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "on two", "--scope", "global", "--plan", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "on nothing", "--scope", "global"}).code == 0);

  CHECK(dispatch(fx, {"question", "list", "--scope", "global", "--plan", "1"}).out == "    1  open        on one\n");
  CHECK(dispatch(fx, {"question", "list", "--scope", "global", "--plan", "2"}).out == "    2  open        on two\n");
  // An inert `--plan` would return all three here.
  CHECK(dispatch(fx, {"question", "list", "--scope", "global"}).out == "    1  open        on one\n"
                                                                       "    2  open        on two\n"
                                                                       "    3  open        on nothing\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|plan|1|derives-from;2|plan|2|derives-from");
  CHECK(question_rows(conn) == "1|global|<NULL>|on one|<NULL>|open|<NULL>|<NULL>;"
                               "2|global|<NULL>|on two|<NULL>|open|<NULL>|<NULL>;"
                               "3|global|<NULL>|on nothing|<NULL>|open|<NULL>|<NULL>");
}

TEST_CASE("question list --touches: unknown repo REFUSES, and the branches differ by scope") {
  auto const fx = make_fixture("listtouches");
  seed(fx);
  REQUIRE(dispatch(fx, {"assoc", "create", "acme", "--kind", "org", "--json"}).code == 0);

  // `init` registered the cwd as repo `proj`. One row scoped directly to
  // it, two reachable only through a touches edge, one unrelated.
  REQUIRE(dispatch(fx, {"question", "add", "direct", "--scope", "repo:proj"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "global edge", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "assoc edge", "--scope", "acme"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "unrelated", "--scope", "global"}).code == 0);
  {
    // `links add` is unported, so the edges are seeded directly. They are
    // fixture state, not the thing under test.
    auto conn = open_db(fx);
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
                             "('question', 2, 'repo', 1, 'touches'), ('question', 3, 'repo', 1, 'touches')");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
  }

  // An unknown repo slug REFUSES. Listing empty would be the silent-filter
  // defect this milestone keeps closing.
  auto const bad = dispatch(fx, {"question", "list", "--touches", "nosuchrepo"});
  CHECK(bad.code == 1);
  CHECK(bad.err == "error: repo 'nosuchrepo' not found\n");

  // THREE DIFFERENT non-empty answers from the same fixture. An inert
  // `--scope` returns the same set three times; a `--scope` applied
  // uniformly to both UNION arms returns EMPTY for the middle two.
  CHECK(dispatch(fx, {"question", "list", "--touches", "proj", "--scope", "repo:proj"}).out == "    1  open        direct\n");
  CHECK(dispatch(fx, {"question", "list", "--touches", "proj", "--scope", "global"}).out == "    2  open        global edge\n");
  CHECK(dispatch(fx, {"question", "list", "--touches", "proj", "--scope", "acme"}).out == "    3  open        assoc edge\n");
  // Two scope members: the direct branch switches back ON (a `repo` ref
  // naming this repo is present) AND the global edge is admitted.
  CHECK(dispatch(fx, {"question", "list", "--touches", "proj", "--scope", "repo:proj,global"}).out ==
        "    1  open        direct\n"
        "    2  open        global edge\n");

  auto conn = open_db(fx);
  // `unrelated` appears in NO result above and is still in the table.
  CHECK(question_rows(conn) == "1|repo|1|direct|<NULL>|open|<NULL>|<NULL>;"
                               "2|global|<NULL>|global edge|<NULL>|open|<NULL>|<NULL>;"
                               "3|association|1|assoc edge|<NULL>|open|<NULL>|<NULL>;"
                               "4|global|<NULL>|unrelated|<NULL>|open|<NULL>|<NULL>");
}

TEST_CASE("question list --json is a single-line array of the show objects") {
  auto const fx = make_fixture("listjson");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "one", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"question", "add", "two", "--scope", "global", "--body", "b"}).code == 0);

  auto const json = dispatch(fx, {"question", "list", "--scope", "global", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.starts_with(R"([{"id":1,)"));
  CHECK(json.out.contains(R"(},{"id":2,)"));
  CHECK(json.out.contains(R"("body":null)"));
  CHECK(json.out.contains(R"("body":"b")"));
  CHECK(json.out.ends_with("]\n"));
  // Exactly ONE newline, at the very end: the array is a single line, which
  // is what every downstream `jq`/NDJSON consumer relies on.
  CHECK(std::ranges::count(json.out, '\n') == 1);
}

// ===========================================================================
// The family's OTHER five leaves, at the boundary
// ===========================================================================

TEST_CASE("the drafting quartet is SERVED, not refused -- and still writes nothing when unanchored") {
  auto const fx = make_fixture("refuse");
  seed(fx);
  REQUIRE(dispatch(fx, {"question", "add", "q", "--scope", "global"}).code == 0);

  // This case pinned all four as exit-64 refusals until plan 996 task 6205
  // landed `editflow`. It is kept, INVERTED, in the same place rather than
  // deleted: the four are asserted SERVED here so the family's refusal set
  // cannot quietly re-absorb one of them, which is the same reason
  // `question link` is asserted working below rather than dropped.
  //
  // The question was added WITHOUT `--plan`, so it has no `derives-from`
  // edge and the anchor resolver cannot place it. That is the interesting
  // half: the verbs RUN, and refuse on the DATA rather than on the build.
  // The two pairs refuse DIFFERENTLY, which is the oracle's asymmetry and
  // not a rounding of it -- see `editflow.cppm`'s header.
  for (auto const* leaf : {"edit", "view"}) {
    auto const r = dispatch(fx, {"question", leaf, "1"});
    INFO("leaf=" << leaf);
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    // Two lines: the prose line `editflow` writes, then the bare Zig error
    // TAG the oracle's un-caught handler lets the runtime print. The oracle
    // follows the tag with seven stack frames naming absolute paths inside
    // its own build tree; this build stops at the tag. THE ONE DELIBERATE
    // DIVERGENCE, recorded in `editflow.cppm` and `dispatch.cpp` too.
    CHECK(r.err == "error: cannot resolve anchor plan for question 1: NoPlanLink\nerror: NoPlanLink\n");
  }
  for (auto const* leaf : {"diff", "review"}) {
    auto const r = dispatch(fx, {"question", leaf, "1"});
    INFO("leaf=" << leaf);
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    // PROSE, naming the family and the id -- these two have `catch` arms
    // where `view`/`edit` have none.
    CHECK(r.err == "error: question 1 is not linked to a plan; cannot resolve anchor plan\n");
  }

  {
    auto conn = open_db(fx);
    // The refusal that had half-written is the worse bug, and it is a LIVE
    // hazard now that these four actually run: `edit` writes the workbench
    // file BEFORE it spawns an editor, and `view` writes it before it
    // spawns a pager. Neither may reach that point without an anchor.
    CHECK(question_rows(conn) == "1|global|<NULL>|q|<NULL>|open|<NULL>|<NULL>");
    CHECK(audit_rows(conn, "question") == "create|1|create question 'q'|<NULL>|<NULL>");
  }

  // `question link` WAS in this list until plan 996 task 6193, which landed
  // the whole entity-link surface at once. It is asserted WORKING here, in
  // the very case that used to pin it as refusing. With task 6205 the other
  // four joined it, so this file now pins ZERO refusals for the family --
  // all ten leaves are served. Its own behaviour is covered in
  // links_leaves.t.cpp; this is the boundary.
  REQUIRE(dispatch(fx, {"plan", "create", "P", "--slug", "p", "--summary", "s", "--scope", "global"}).code == 0);
  auto const link = dispatch(fx, {"question", "link", "1", "plan:1", "--relationship", "derives-from"});
  CHECK(link.code == 0);
  CHECK(link.err.empty());
  CHECK(link.out == "linked question:1 -> plan:1  [derives-from]  (link id: 1)\n");

  auto conn = open_db(fx);
  // The fixture carries NO pre-existing edge (`question add` ran without
  // `--plan`), so this is the first and only row.
  CHECK(edge_rows(conn) == "1|plan|1|derives-from");
}

// ===========================================================================
// assoc members (plan 996, task 6188)
//
// Wired in the same cycle as the `question` family, and tested here rather
// than in a file of its own for one reason: it is ONE leaf. The engine call
// (`identity::members`) already existed; the missing piece was a
// `project_ref` list renderer, and the reason it is worth a cycle's tail is
// that five `groups_recommend_test` integration frames were crashing in
// their FIXTURE on `assoc members --json` rather than in `groups recommend`.
//
//   $Z assoc members org       (no members) b'(no members)\n'
//   $Z assoc members org       b'_                     .\n'
//   $Z assoc members nope      exit 1  b"error: no association named 'nope'\n"
//   $Z assoc members org       with a NULL root_path:
//                              b'nullroot              (no root)\n'
// ===========================================================================

TEST_CASE("assoc members: an unknown association REFUSES, an empty one prints a word") {
  auto const fx = make_fixture("members");
  seed(fx);
  REQUIRE(dispatch(fx, {"assoc", "create", "org", "--kind", "org", "--json"}).code == 0);

  // These two outcomes MUST differ. Returning `(no members)` for an
  // unknown slug is the silent-empty defect: it reads as "this association
  // has no members" for an association that does not exist.
  auto const unknown = dispatch(fx, {"assoc", "members", "nope"});
  CHECK(unknown.code == 1);
  CHECK(unknown.out.empty());
  CHECK(unknown.err == "error: no association named 'nope'\n");
  // ...and `--json` does NOT switch it to an empty array.
  auto const unknown_json = dispatch(fx, {"assoc", "members", "nope", "--json"});
  CHECK(unknown_json.code == 1);
  CHECK(unknown_json.out == planar::cmd::testsupport::json_error_envelope_line("assoc members", "generic_failure"));
  CHECK(unknown_json.err == "error: no association named 'nope'\n");

  auto const empty = dispatch(fx, {"assoc", "members", "org"});
  CHECK(empty.code == 0);
  CHECK(empty.out == "(no members)\n");
  auto const empty_json = dispatch(fx, {"assoc", "members", "org", "--json"});
  CHECK(empty_json.code == 0);
  CHECK(empty_json.out == "[]\n");
}

TEST_CASE("assoc members lists only THIS association's members, and the others survive") {
  auto const fx = make_fixture("membersfilter");
  seed(fx);
  REQUIRE(dispatch(fx, {"assoc", "create", "org", "--kind", "org", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "other", "--kind", "org", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "org", "/w/alpha", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "other", "/w/zebra", "--json"}).code == 0);

  // The membership filter EXCLUDES: `zebra` is in the database and is not
  // in this listing, and vice versa.
  auto const mine = dispatch(fx, {"assoc", "members", "org"});
  CHECK(mine.code == 0);
  CHECK(mine.out == "alpha                 /w/alpha\n");
  auto const theirs = dispatch(fx, {"assoc", "members", "other"});
  CHECK(theirs.code == 0);
  CHECK(theirs.out == "zebra                 /w/zebra\n");

  auto conn = open_db(fx);
  // Both survive; the listing filtered, it did not delete.
  CHECK(query_rows(conn, "select slug, name, root_path from projects where slug in ('alpha','zebra') order by slug", 3) ==
        "alpha|alpha|/w/alpha;zebra|zebra|/w/zebra");
}

TEST_CASE("assoc members renders root_path (not name) and `(no root)` for NULL") {
  auto const fx = make_fixture("membersnull");
  seed(fx);
  REQUIRE(dispatch(fx, {"assoc", "create", "org", "--kind", "org", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "org", "/w/widgets", "--json"}).code == 0);
  {
    // A project row with a NULL `root_path` is unreachable through the
    // CLI, so it is seeded directly. It is fixture state, not the thing
    // under test — but the state is real, and a renderer that printed an
    // empty column for it would be indistinguishable from `root_path = ''`.
    auto conn = open_db(fx);
    auto stmt = conn.prepare("insert into projects (slug, name) values ('nullroot', 'Null Root')");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    auto link = conn.prepare("insert into project_associations (project_id, association_id, source) "
                             "select (select id from projects where slug = 'nullroot'), "
                             "(select id from associations where slug = 'org'), 'user'");
    REQUIRE(link.has_value());
    REQUIRE(link->step().has_value());
  }

  auto const text = dispatch(fx, {"assoc", "members", "org"});
  CHECK(text.code == 0);
  CHECK(text.out == "nullroot              (no root)\n"
                    "widgets               /w/widgets\n");

  auto const json = dispatch(fx, {"assoc", "members", "org", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.contains(R"("slug":"nullroot","name":"Null Root","root_path":null)"));
  CHECK(json.out.contains(R"("slug":"widgets","name":"widgets","root_path":"/w/widgets")"));
  // One newline, at the very end.
  CHECK(std::ranges::count(json.out, '\n') == 1);
}
