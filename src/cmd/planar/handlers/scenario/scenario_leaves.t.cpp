// @file scenario_leaves.t.cpp
// @brief In-process tests for the six leaves wired by plan 996, task 6195:
// `scenario add`, `show`, `list`, `verify`, `retire` and `link`.
//
// Its own file rather than more of `handlers.t.cpp` (3.8k lines already),
// on the discipline `annotate_leaves.t.cpp`, `question_leaves.t.cpp` and
// `decision_leaves.t.cpp` established.
//
// ## EVERY CASE ASSERTS DATABASE ROWS
//
// Stdout is checked where the bytes are the contract, but no case rests on
// stdout alone. The defect class this milestone keeps finding is a verb
// that exits 0 with oracle-identical output and wrong rows. So every
// mutation here is followed by a `scenario_rows` / `audit_rows` /
// `edge_rows` snapshot, and every REFUSAL is followed by one too — a
// refusal that half-wrote is the worse bug, and only the after-state
// distinguishes it.
//
// SQL NULL renders as the literal `<NULL>` and is never collapsed onto the
// empty string. `test_scenarios.body` is genuinely nullable and an absent
// `--body` must produce NULL, not `""` — while `--body ""` must produce
// `''`. Both directions are asserted, at the row level AND at the rendered
// level, because the two differ by a whole LINE of `scenario show` output
// and a row snapshot alone cannot see a renderer that collapses them.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, BY A SURVIVOR
//
// Each `scenario list` case seeds rows on BOTH sides of its predicate and
// asserts the returned set AND that the excluded rows are still in the
// table afterwards. `--touches` gets the sharpest discrimination available:
// the SAME repo under two different scopes returns two DISJOINT sets,
// neither a subset of the other and neither equal to the unfiltered union.
// An inert scope filter gives one answer for all three; a
// uniformly-applied one collapses two of them to empty.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string was captured from `zig/zig-out/bin/planar`
// against a scratch `PLANAR_DB`, read back through `od -c` rather than
// through a pipe into `tail`. The whole set was then re-derived as a
// SEQUENCE diff: a 63-step argv script replayed against BOTH binaries in
// identically-named scratch roots, with stdout, stderr, exit codes AND the
// resulting `test_scenarios` / `entity_links` / `audit_log` / `sessions` /
// `agent_actions` / `artifacts` / `plans` dumps compared row by row.
// EXACTLY TWO steps differed, both pre-existing and named below. The
// captures that decided a shape:
//
//   $Z scenario list --scope global    stdout b'(no scenarios)\n'
//       ^ WITH parentheses. `decision` emits a bare `no decisions`.
//   $Z scenario list --scope global    (draft + verified + retired seeded)
//       b'    1  verified    pass      T1\n    2  draft       fail ...'
//       ^ EMPTY `--status` returns EVERY status, `retired` included.
//         `question`'s empty arm means `open`; `decision`'s means
//         `{proposed, accepted}`. Three families, three answers.
//       ^ FOUR columns: id, status, OUTCOME, title. A null outcome is a
//         `-` padded to eight.
//   $Z scenario list --status bogus     exit **1**
//       ^ the identical refusal on `decision list` exits 2.
//   $Z scenario list --status draft,verified   BOTH returned
//       ^ comma-split, unlike `decision list`.
//   $Z scenario add x --related 77      exit 1  b'scenario add: QueryFailed'
//       ^ a real column FK. NO row written.
//   $Z scenario add x --plan 4242       exit 0
//       ^ a DANGLING edge, `from_kind = 'test_scenario'`. The two
//         link-ish flags on ONE verb behave OPPOSITELY.
//   $Z scenario verify <retired>        exit 1  b'scenario verify: IllegalTransition'
//       ^ and `updated_at` UNMOVED. The same row accepts
//         `--outcome skipped` at exit 0 — the matrix is consulted only on
//         the pass path.
//   $Z scenario verify <draft>          two audit rows:
//       'ready: auto-transition via verify' then 'verify: pass'
//   $Z scenario verify 2 --outcome fail --summary broke
//       audit summary b'verify(fail): broke'
//       ^ the outcome moves INSIDE the parentheses; it is not appended.
//   $Z scenario add x --editor          exit 0, and stderr carries
//       b'warning: --editor not yet implemented; falling back to inline create\n'
//       ^ the ORACLE's own warning, not this port's compensation.
//   $Z scenario link 4 plan:1 --relationship verifies
//       b'linked test_scenario:4 -> plan:1  [verifies]  (link id: N)\n'
//       ^ the subject renders as `test_scenario` though the VERB is
//         `scenario` and the audit entity kind is `scenario`.
//
// ## The two sequence-diff divergences, both pre-existing
//
//  1. `scenario add` with no positional reports `title is required` /
//     `RequiredError` where the oracle reports `required positional
//     missing: <title>` / `MissingRequiredPositional`. That is the shared
//     CLI11 parser layer's wording for EVERY required positional in the
//     binary — verified identical on `plan create`, `question add` and
//     `task show`, all already ported — and changing it here would be a
//     tree-wide change smuggled into one family. The EXIT CODE (2) matches.
//  2. `scenario add --related <nonexistent>` prints TWO stderr lines in the
//     oracle, the first being a `std.log.err` (`scenario.create exec
//     failed: StepFailed`). No engine module in this tree imports
//     `planar.log` at all, so no port reproduces Zig's log lines; the
//     refusal line itself is byte-identical.
//
// A third difference is NOT a divergence but a scope boundary: `artifact
// add` is unported, so the sequence diff seeds `artifacts` by SQL. The
// `--related` cases below do the same.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

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
                         std::format("planar_sc_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Run a statement against the fixture database, for seeding rows
/// whose creating verb is unported (`artifacts`) or unreachable (`failing`).
/// @param conn An open connection to the fixture database.
/// @param sql The statement. A test-local literal, never operator input.
void exec(planar::db::connection& conn, std::string_view sql) {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Every `test_scenarios` row as
/// `id|scope_kind|scope_id|title|body|status|related_artifact_id|last_run_at|last_outcome`.
///
/// `last_run_at` is a wall-clock timestamp, so it is projected as the
/// two-state `<NULL>` / `SET`; whether the column is null is the contract
/// (every `verify` writes it, `retire` must not), its value is not.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto scenario_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select id, scope_kind, scope_id, title, body, status, related_artifact_id, "
                    "case when last_run_at is null then null else 'SET' end, last_outcome "
                    "from test_scenarios order by id",
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

/// @brief Every `test_scenario -> *` edge as
/// `from_id|to_kind|to_id|relationship`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto edge_rows(planar::db::connection& conn) -> std::string {
  return query_rows(
      conn, "select from_id, to_kind, to_id, relationship from entity_links where from_kind = 'test_scenario' order by id", 4);
}

/// @brief Bring a fixture up to an initialised database with `proj`
/// registered as a project.
/// @param fx The fixture.
void seed(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
}

/// @brief Create a scenario through the CLI, requiring success.
/// @param fx The fixture.
/// @param args The argv tail after `scenario add`.
void add(const fixture& fx, std::vector<std::string> args) {
  std::vector<std::string> argv{"scenario", "add"};
  argv.insert(argv.end(), args.begin(), args.end());
  auto const res = dispatch(fx, argv);
  INFO(res.err);
  REQUIRE(res.code == 0);
}

} // namespace

// ===========================================================================
// scenario add
// ===========================================================================

TEST_CASE("scenario add writes a draft row and echoes the oracle's block") {
  auto const fx = make_fixture("add");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "add", "Happy path: first", "--scope", "global"});
  REQUIRE(res.code == 0);
  // Values start at column 13. Everything but the two timestamps is fixed,
  // so the block is checked in halves rather than loosened to a `contains`.
  CHECK(res.out.starts_with("id:         1\n"
                            "title:      Happy path: first\n"
                            "status:     draft\n"
                            "scope:      global\n"
                            "created:    "));
  CHECK(res.out.ends_with("Z\n"));
  CHECK(res.err.empty());

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|Happy path: first|<NULL>|draft|<NULL>|<NULL>|<NULL>");
  // The audit entity kind is `scenario`, NOT `test_scenario`. The
  // entity_links `from_kind` this family writes IS `test_scenario`; the two
  // spellings genuinely differ and both are captured.
  CHECK(audit_rows(conn, "scenario") == "create|1|create scenario 'Happy path: first'|<NULL>|<NULL>");
  CHECK(edge_rows(conn).empty());
  // NO SESSION, unlike `question add` / `decision add`. zig's
  // scenario/add.zig calls neither `ensureActive` nor the entity-create
  // activity hook, and `test_scenarios` has no `session_id` column. A port
  // that copied `decision add` wholesale would write a row here.
  CHECK(query_rows(conn, "select count(*) from sessions", 1) == "0");
  CHECK(query_rows(conn, "select count(*) from agent_actions", 1) == "0");
}

TEST_CASE("scenario add cwd-derives global inside a registered, UNASSOCIATED project") {
  auto const fx = make_fixture("add-cwd");
  seed(fx);

  // No `--scope`. `proj` IS registered but has no association, and unlike
  // `plan create` this verb does not refuse there — the row lands `global`.
  auto const res = dispatch(fx, {"scenario", "add", "derived"});
  REQUIRE(res.code == 0);
  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|derived|<NULL>|draft|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("scenario add distinguishes an ABSENT body from an EMPTY one, in rows AND output") {
  auto const fx = make_fixture("add-body");
  seed(fx);

  add(fx, {"absent", "--scope", "global"});
  add(fx, {"empty", "--body", "", "--scope", "global"});

  auto conn = open_db(fx);
  // At the ROW level, `<NULL>` versus the empty rendering. The two differ
  // in this snapshot by exactly one field, which is the point — note the
  // `|<NULL>|draft|` of row 1 against the `||draft|` of row 2.
  CHECK(scenario_rows(conn) == "1|global|<NULL>|absent|<NULL>|draft|<NULL>|<NULL>|<NULL>;"
                               "2|global|<NULL>|empty||draft|<NULL>|<NULL>|<NULL>");

  // A `|`-joined snapshot is a weak place to read an empty field, so the
  // same distinction is re-asserted as an explicit two-state projection,
  // and then through the RENDERED form, where the difference is a whole
  // LINE — a class no raw-SQL snapshot can see at all.
  CHECK(query_rows(conn,
                   "select id, case when body is null then 'NULL' else 'TEXT[' || body || ']' end "
                   "from test_scenarios order by id",
                   2) == "1|NULL;2|TEXT[]");

  auto const absent = dispatch(fx, {"scenario", "show", "1"});
  CHECK(absent.code == 0);
  CHECK_FALSE(absent.out.contains("body:"));
  auto const empty = dispatch(fx, {"scenario", "show", "2"});
  CHECK(empty.code == 0);
  CHECK(empty.out.contains("body:       \n"));
  // ...and in JSON, `null` versus `""`.
  CHECK(dispatch(fx, {"scenario", "show", "1", "--json"}).out.contains(R"("body":null)"));
  CHECK(dispatch(fx, {"scenario", "show", "2", "--json"}).out.contains(R"("body":"")"));
}

TEST_CASE("scenario add --related a NONEXISTENT artifact refuses at exit 1 with NO row") {
  auto const fx = make_fixture("add-related-bad");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "add", "bad", "--related", "77", "--scope", "global"});
  CHECK(res.code == 1);
  // ORACLE, verbatim (its second, `std.log.err` line is the known
  // divergence this file's header names).
  CHECK(res.err == "error: scenario add: QueryFailed\n");
  CHECK(res.out.empty());

  auto conn = open_db(fx);
  // The whole after-state: the FK failed the INSERT, so not even a partial
  // row landed, and the audit write is downstream of it.
  CHECK(scenario_rows(conn).empty());
  CHECK(audit_rows(conn, "scenario").empty());
}

TEST_CASE("scenario add --plan a NONEXISTENT plan refuses and writes NOTHING") {
  auto const fx = make_fixture("add-plan-dangling");
  seed(fx);

  // Task 6197. This used to be the OPPOSITE answer to `--related` on the
  // same verb: `--related 77` refused (a real FK on `related_artifact_id`)
  // while `--plan 4242` exited 0 and left an `entity_links` edge behind,
  // because `entity_links` has no FK to its target table. One verb, two
  // reference flags, two answers. Both endpoints now refuse.
  auto const res = dispatch(fx, {"scenario", "add", "P-only", "--plan", "4242", "--scope", "global"});
  CHECK(res.code == 1);

  auto conn = open_db(fx);
  // THE ASSERTION THAT MATTERS: the original defect exited 0, so only the
  // absence of the edge row distinguishes the fix from the bug.
  CHECK(edge_rows(conn).empty());
  CHECK(scenario_rows(conn).empty());
  CHECK(query_rows(conn, "select count(*) from plans", 1) == "0");
  CHECK(audit_rows(conn, "scenario").empty());
  CHECK(audit_rows(conn, "entity_link").empty());
}

TEST_CASE("scenario add --plan an EXISTING plan writes the test_scenario edge") {
  auto const fx = make_fixture("add-plan-ok");
  seed(fx);
  // Seeded THROUGH THE CLI so the anchor is the row an operator would have.
  REQUIRE(dispatch(fx, {"plan", "create", "anchor", "--scope", "global"}).code == 0);

  auto const res = dispatch(fx, {"scenario", "add", "P-only", "--plan", "1", "--scope", "global"});
  CHECK(res.code == 0);

  auto conn = open_db(fx);
  // The edge is REALLY there and its `from_kind` is `test_scenario`.
  CHECK(edge_rows(conn) == "1|plan|1|derives-from");
  // Exactly ONE audit row and its verb is `create` — the edge gets none.
  CHECK(audit_rows(conn, "scenario") == "create|1|create scenario 'P-only'|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "entity_link").empty());
}

TEST_CASE("scenario add --related an EXISTING artifact stores it and renders the artifact line") {
  auto const fx = make_fixture("add-related-ok");
  seed(fx);
  {
    // `artifact add` is unported, so the row is seeded directly.
    auto conn = open_db(fx);
    exec(conn, "insert into artifacts (scope_kind, scope_id, kind, title) values ('global', null, 'test_spec', 'spec A')");
  }

  auto const res = dispatch(fx, {"scenario", "add", "linked", "--related", "1", "--scope", "global"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains("artifact:   1\n"));

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|linked|<NULL>|draft|1|<NULL>|<NULL>");
}

TEST_CASE("scenario add --scope an unresolvable slug refuses before any write") {
  auto const fx = make_fixture("add-badscope");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "add", "x", "--scope", "nosuchslug"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: scenario add: SlugNotFound\n");

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn).empty());
  CHECK(audit_rows(conn, "scenario").empty());
  // ...and no session either — this verb starts none on ANY path.
  CHECK(query_rows(conn, "select count(*) from sessions", 1) == "0");
}

TEST_CASE("scenario add --editor WARNS on stderr and proceeds") {
  auto const fx = make_fixture("add-editor");
  seed(fx);

  // The ORACLE prints this same line and falls through to the same inline
  // create — unlike `decision add --editor`, where the warning is this
  // port's compensation for an unported flow. So stderr is byte-identical
  // here, not merely stdout.
  auto const res = dispatch(fx, {"scenario", "add", "Editor one", "--editor", "--scope", "global"});
  CHECK(res.code == 0);
  CHECK(res.err == "warning: --editor not yet implemented; falling back to inline create\n");

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|Editor one|<NULL>|draft|<NULL>|<NULL>|<NULL>");
}

// ===========================================================================
// scenario show
// ===========================================================================

TEST_CASE("scenario show reports the oracle's not-found message at exit 1") {
  auto const fx = make_fixture("show-missing");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "show", "999"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: no scenario with id 999\n");
  CHECK(res.out.empty());
}

TEST_CASE("scenario show refuses a non-integer id at exit 2") {
  auto const fx = make_fixture("show-badid");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "show", "abc"});
  CHECK(res.code == 2);
  CHECK(res.err == "error: scenario id must be an integer, got 'abc'\n");
}

TEST_CASE("scenario show renders every conditional line in the oracle's ORDER") {
  auto const fx = make_fixture("show-full");
  seed(fx);
  {
    auto conn = open_db(fx);
    exec(conn, "insert into artifacts (scope_kind, scope_id, kind, title) values ('global', null, 'test_spec', 'spec A')");
  }
  add(fx, {"Edge: with body", "--body", "Given X when Y then Z", "--related", "1", "--scope", "global"});
  REQUIRE(dispatch(fx, {"scenario", "verify", "1", "--outcome", "error"}).code == 0);

  auto const res = dispatch(fx, {"scenario", "show", "1"});
  REQUIRE(res.code == 0);
  // `artifact:` then `outcome:` then `last run:` then `body:` — body LAST,
  // after the run columns. `decision`'s unconditional `body:` comes first,
  // so a copy from there reorders every line of this block.
  auto const artifact = res.out.find("artifact:");
  auto const outcome  = res.out.find("outcome:");
  auto const last_run = res.out.find("last run:");
  auto const body     = res.out.find("body:");
  auto const created  = res.out.find("created:");
  REQUIRE(artifact != std::string::npos);
  CHECK(artifact < outcome);
  CHECK(outcome < last_run);
  CHECK(last_run < body);
  CHECK(body < created);
  CHECK(res.out.contains("outcome:    error\n"));
}

// ===========================================================================
// scenario verify
// ===========================================================================

TEST_CASE("scenario verify defaults to PASS and auto-transitions a draft through ready") {
  auto const fx = make_fixture("verify-auto");
  seed(fx);
  add(fx, {"auto", "--scope", "global"});

  // No `--outcome` at all: the default is applied by the handler.
  auto const res = dispatch(fx, {"scenario", "verify", "1"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains("status:     verified\n"));
  CHECK(res.out.contains("outcome:    pass\n"));

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|auto|<NULL>|verified|<NULL>|SET|pass");
  // The intermediate row is the only observable proof the two-hop walk
  // happened rather than a direct UPDATE. A final-status assertion alone
  // passes either way.
  CHECK(audit_rows(conn, "scenario") == "create|1|create scenario 'auto'|<NULL>|<NULL>;"
                                        "status_change|1|ready: auto-transition via verify|<NULL>|<NULL>;"
                                        "status_change|1|verify: pass|<NULL>|<NULL>");
}

TEST_CASE("scenario verify --outcome fail records the run and leaves status ALONE") {
  auto const fx = make_fixture("verify-fail");
  seed(fx);
  add(fx, {"T2", "--scope", "global"});

  auto const res = dispatch(fx, {"scenario", "verify", "1", "--outcome", "fail", "--summary", "broke"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains("status:     draft\n"));

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|T2|<NULL>|draft|<NULL>|SET|fail");
  // ORACLE: the outcome moves INSIDE the parentheses when a summary is
  // present. `verify: fail: broke` is the natural guess and is wrong.
  CHECK(audit_rows(conn, "scenario") == "create|1|create scenario 'T2'|<NULL>|<NULL>;"
                                        "status_change|1|verify(fail): broke|<NULL>|<NULL>");
}

TEST_CASE("scenario verify PASS on a RETIRED row refuses, and updated_at does NOT move") {
  auto const fx = make_fixture("verify-retired");
  seed(fx);
  add(fx, {"gone", "--scope", "global"});
  REQUIRE(dispatch(fx, {"scenario", "retire", "1"}).code == 0);

  std::string before;
  std::string audit_before;
  {
    auto conn    = open_db(fx);
    before       = query_rows(conn, "select updated_at from test_scenarios where id = 1", 1);
    audit_before = audit_rows(conn, "scenario");
  }

  auto const res = dispatch(fx, {"scenario", "verify", "1"});
  CHECK(res.code == 1);
  // UNFOLDED — the operator sees the bare Zig tag. `decision` humanises the
  // same class of refusal into `is terminal; cannot accept`; this family
  // has no humanised transition refusal at all.
  CHECK(res.err == "error: scenario verify: IllegalTransition\n");
  CHECK(res.out.empty());

  auto conn = open_db(fx);
  // The whole after-state. `updated_at` is the assertion a status-only
  // check would miss: a port that ran the UPDATE and then refused would
  // still report `retired` here.
  CHECK(scenario_rows(conn) == "1|global|<NULL>|gone|<NULL>|retired|<NULL>|<NULL>|<NULL>");
  CHECK(query_rows(conn, "select updated_at from test_scenarios where id = 1", 1) == before);
  CHECK(audit_rows(conn, "scenario") == audit_before);
}

TEST_CASE("scenario verify --outcome skipped on the SAME retired row SUCCEEDS") {
  auto const fx = make_fixture("verify-retired-skip");
  seed(fx);
  add(fx, {"gone", "--scope", "global"});
  REQUIRE(dispatch(fx, {"scenario", "retire", "1"}).code == 0);

  // The complement of the case above. Whether a `verify` is refused depends
  // entirely on the OUTCOME, not on the status — a port that gated the
  // whole verb on the matrix passes the refusal case and fails this one.
  auto const res = dispatch(fx, {"scenario", "verify", "1", "--outcome", "skipped"});
  CHECK(res.code == 0);

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|gone|<NULL>|retired|<NULL>|SET|skipped");
}

TEST_CASE("scenario verify refuses an unknown --outcome at exit 2 with nothing written") {
  auto const fx = make_fixture("verify-badoutcome");
  seed(fx);
  add(fx, {"x", "--scope", "global"});

  auto const res = dispatch(fx, {"scenario", "verify", "1", "--outcome", "bogus"});
  CHECK(res.code == 2);
  // Exit 2 here, where the sibling `list` leaf's `--status bogus` exits 1.
  // Both captured.
  CHECK(res.err == "error: unknown outcome 'bogus' (want pass|fail|error|skipped)\n");

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|x|<NULL>|draft|<NULL>|<NULL>|<NULL>");
  CHECK(audit_rows(conn, "scenario") == "create|1|create scenario 'x'|<NULL>|<NULL>");
}

TEST_CASE("scenario verify on an absent id reports not-found and writes nothing") {
  auto const fx = make_fixture("verify-missing");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "verify", "999"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: no scenario with id 999\n");

  auto conn = open_db(fx);
  CHECK(audit_rows(conn, "scenario").empty());
}

// ===========================================================================
// scenario retire
// ===========================================================================

TEST_CASE("scenario retire flips the status, keeps the run columns, and repeats") {
  auto const fx = make_fixture("retire");
  seed(fx);
  add(fx, {"old test", "--scope", "global"});
  REQUIRE(dispatch(fx, {"scenario", "verify", "1"}).code == 0);

  auto const first = dispatch(fx, {"scenario", "retire", "1", "--reason", "feature removed"});
  REQUIRE(first.code == 0);
  CHECK(first.out.contains("status:     retired\n"));

  {
    auto conn = open_db(fx);
    // `last_outcome` SURVIVES retirement. A port that cleared it would
    // still report `retired` and pass a status-only check.
    CHECK(scenario_rows(conn) == "1|global|<NULL>|old test|<NULL>|retired|<NULL>|SET|pass");
    CHECK(query_rows(conn, "select summary from audit_log order by id desc limit 1", 1) == "retire: feature removed");
  }

  // `retired -> retired` short-circuits, so a second retire SUCCEEDS and
  // writes a second row — this one with the BARE summary.
  auto const second = dispatch(fx, {"scenario", "retire", "1"});
  CHECK(second.code == 0);
  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select summary from audit_log order by id desc limit 1", 1) == "retire");
  CHECK(query_rows(conn, "select count(*) from audit_log where summary like 'retire%'", 1) == "2");
}

TEST_CASE("scenario retire on an absent id reports not-found") {
  auto const fx = make_fixture("retire-missing");
  seed(fx);

  auto const res = dispatch(fx, {"scenario", "retire", "999"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: no scenario with id 999\n");
}

// ===========================================================================
// scenario list
// ===========================================================================

TEST_CASE("scenario list renders '(no scenarios)' for text and '[]' for json") {
  auto const fx = make_fixture("list-empty");
  seed(fx);

  auto const text = dispatch(fx, {"scenario", "list", "--scope", "global"});
  CHECK(text.code == 0);
  // WITH parentheses. `decision list` emits a bare `no decisions` for the
  // same shape of emptiness, and the two disagree.
  CHECK(text.out == "(no scenarios)\n");

  auto const json = dispatch(fx, {"scenario", "list", "--scope", "global", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "[]\n");
}

TEST_CASE("scenario list refuses outright when the cwd is in no registered scope") {
  auto const fx = make_fixture("list-noscope");
  seed(fx);

  auto const res = dispatch_in(fx, "outside", {"scenario", "list"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global\n");
  CHECK(res.out.empty());
}

TEST_CASE("scenario list --status: seven queries, seven answers, excluded rows survive") {
  auto const fx = make_fixture("list-status");
  seed(fx);
  {
    // Seeded by SQL so the fixture says what it means and `failing` — which
    // NO scenario verb can reach — is represented too.
    auto conn = open_db(fx);
    exec(conn, "insert into test_scenarios (scope_kind, scope_id, title, status) values "
               "('global', null, 'd', 'draft'), "
               "('global', null, 'r', 'ready'), "
               "('global', null, 'v', 'verified'), "
               "('global', null, 'f', 'failing'), "
               "('global', null, 'x', 'retired')");
  }

  auto const titles = [&](std::vector<std::string> args) {
    auto const res = dispatch(fx, args);
    INFO(res.err);
    REQUIRE(res.code == 0);
    std::string out;
    for (auto const line : std::views::split(std::string_view{res.out}, '\n')) {
      std::string_view sv{line.begin(), line.end()};
      if (sv.empty()) {
        continue;
      }
      if (!out.empty()) {
        out += ",";
      }
      out += sv.substr(sv.find_last_of(' ') + 1);
    }
    return out;
  };

  // 1. EMPTY means EVERY status, `retired` included. That is this family's
  //    own answer: `question`'s empty arm means `open`, `decision`'s means
  //    `{proposed, accepted}`, `plan`'s meaning "everything" was a bug.
  CHECK(titles({"scenario", "list", "--scope", "global"}) == "d,r,v,f,x");
  // 2.-6. One status each. Five DIFFERENT single-title answers, so an inert
  //    filter (which would return the full set five times) fails five times.
  CHECK(titles({"scenario", "list", "--scope", "global", "--status", "draft"}) == "d");
  CHECK(titles({"scenario", "list", "--scope", "global", "--status", "ready"}) == "r");
  CHECK(titles({"scenario", "list", "--scope", "global", "--status", "verified"}) == "v");
  CHECK(titles({"scenario", "list", "--scope", "global", "--status", "failing"}) == "f");
  CHECK(titles({"scenario", "list", "--scope", "global", "--status", "retired"}) == "x");
  // 7. COMMA-SPLIT, unlike `decision list --status`, where the same token
  //    would be one unknown status.
  CHECK(titles({"scenario", "list", "--scope", "global", "--status", "draft,retired"}) == "d,x");

  auto conn = open_db(fx);
  // The EXCLUDED rows SURVIVE. A filter that deleted its non-matches would
  // pass every assertion above.
  CHECK(query_rows(conn, "select id, status from test_scenarios order by id", 2) ==
        "1|draft;2|ready;3|verified;4|failing;5|retired");
}

TEST_CASE("scenario list --status bogus exits 1, where decision's identical refusal exits 2") {
  auto const fx = make_fixture("list-badstatus");
  seed(fx);
  add(fx, {"survivor", "--scope", "global"});

  auto const res = dispatch(fx, {"scenario", "list", "--scope", "global", "--status", "bogus"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: unknown status 'bogus'\n");
  CHECK(res.out.empty());

  auto conn = open_db(fx);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|survivor|<NULL>|draft|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("scenario list --scope: three kinds, five queries, and the survivors are checked") {
  auto const fx = make_fixture("list-scope");
  seed(fx);
  {
    auto conn = open_db(fx);
    exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  }
  add(fx, {"g", "--scope", "global"});
  add(fx, {"a", "--scope", "acme"});
  add(fx, {"r", "--scope", "repo:proj"});

  auto const ids = [&](std::vector<std::string> args) {
    auto const res = dispatch(fx, args);
    INFO(res.err);
    REQUIRE(res.code == 0);
    std::string out;
    for (auto const line : std::views::split(std::string_view{res.out}, '\n')) {
      std::string_view sv{line.begin(), line.end()};
      if (sv.empty()) {
        continue;
      }
      if (!out.empty()) {
        out += ",";
      }
      out += sv.substr(sv.find_last_of(' ') + 1);
    }
    return out;
  };

  CHECK(ids({"scenario", "list", "--scope", "global"}) == "g");
  CHECK(ids({"scenario", "list", "--scope", "acme"}) == "a");
  CHECK(ids({"scenario", "list", "--scope", "repo:proj"}) == "r");
  // COMMA-SPLIT, unlike `decision list --scope`, where this exact token is
  // one slug and fails `SlugNotFound`.
  CHECK(ids({"scenario", "list", "--scope", "global,repo:proj"}) == "g,r");
  // No `--scope` at all: the cwd-derived READ SET, which for a registered
  // project with no association is the repo alone — the global row is
  // EXCLUDED, which is what makes this a filter rather than a no-op.
  CHECK(ids({"scenario", "list"}) == "r");

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, scope_kind from test_scenarios order by id", 2) == "1|global;2|association;3|repo");
}

TEST_CASE("scenario list --scope an unresolvable slug fails the WHOLE call") {
  auto const fx = make_fixture("list-badscope");
  seed(fx);
  add(fx, {"survivor", "--scope", "global"});

  // Dropping the bad member and returning the good one's rows would look
  // like success and be a silently short list.
  auto const res = dispatch(fx, {"scenario", "list", "--scope", "global,nosuchslug"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: scenario list: SlugNotFound\n");
  CHECK(res.out.empty());
}

TEST_CASE("scenario list --related: three queries, three answers, non-matches survive") {
  auto const fx = make_fixture("list-related");
  seed(fx);
  {
    auto conn = open_db(fx);
    exec(conn, "insert into artifacts (scope_kind, scope_id, kind, title) values "
               "('global', null, 'test_spec', 'A'), ('global', null, 'tech_spec', 'B')");
  }
  add(fx, {"for A", "--related", "1", "--scope", "global"});
  add(fx, {"for B", "--related", "2", "--scope", "global"});
  add(fx, {"for none", "--scope", "global"});

  auto const out = [&](std::vector<std::string> args) {
    auto const res = dispatch(fx, args);
    INFO(res.err);
    REQUIRE(res.code == 0);
    return res.out;
  };

  CHECK(out({"scenario", "list", "--scope", "global"}).contains("for none"));
  CHECK(out({"scenario", "list", "--scope", "global", "--related", "1"}) == "    1  draft       -         for A\n");
  CHECK(out({"scenario", "list", "--scope", "global", "--related", "2"}) == "    2  draft       -         for B\n");
  // A NULL `related_artifact_id` is matched by NO id — `= ?` is never true
  // against NULL, so scenario 3 is unreachable through this filter.
  CHECK(out({"scenario", "list", "--scope", "global", "--related", "99"}) == "(no scenarios)\n");

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, related_artifact_id from test_scenarios order by id", 2) == "1|1;2|2;3|<NULL>");
}

TEST_CASE("scenario list renders the four-column table with a dash for a null outcome") {
  auto const fx = make_fixture("list-columns");
  seed(fx);
  add(fx, {"T1", "--scope", "global"});
  add(fx, {"T2", "--scope", "global"});
  REQUIRE(dispatch(fx, {"scenario", "verify", "1"}).code == 0);

  auto const res = dispatch(fx, {"scenario", "list", "--scope", "global"});
  REQUIRE(res.code == 0);
  // ORACLE, byte for byte. FOUR columns — a copy of `decision`'s
  // `{:>5}  {:<10}  {}` drops the outcome and left-shifts every title.
  CHECK(res.out == "    1  verified    pass      T1\n"
                   "    2  draft       -         T2\n");
}

// ===========================================================================
// scenario list --touches
// ===========================================================================

TEST_CASE("scenario list --touches: one repo, two scopes, two DISJOINT sets") {
  auto const fx = make_fixture("list-touches");
  seed(fx);
  add(fx, {"in repo", "--scope", "repo:proj"}); // id 1 — direct arm only
  add(fx, {"touches", "--scope", "global"});    // id 2 — touches arm only
  add(fx, {"unrelated", "--scope", "global"});  // id 3 — neither
  REQUIRE(dispatch(fx, {"scenario", "link", "2", "repo:1", "--relationship", "touches"}).code == 0);

  auto const out = [&](std::vector<std::string> args) {
    auto const res = dispatch(fx, args);
    INFO(res.err);
    REQUIRE(res.code == 0);
    return res.out;
  };

  // No `--scope`: the cwd read set is `repo:proj`, so the direct arm
  // survives the all-or-nothing gate and the touches arm's GLOBAL row is
  // filtered out.
  CHECK(out({"scenario", "list", "--touches", "proj"}) == "    1  draft       -         in repo\n");
  // `--scope global`: the direct arm is switched OFF entirely (`1 = 0`) and
  // the touches arm keeps its global row.
  //
  // These two sets are DISJOINT — neither a subset of the other. An inert
  // scope filter would return the same answer twice; a uniformly-applied
  // one would collapse one of them to empty. Only the real two-arm
  // behaviour produces both.
  CHECK(out({"scenario", "list", "--touches", "proj", "--scope", "global"}) == "    2  draft       -         touches\n");
  // ...and the status filter still composes on top of the touches arm.
  CHECK(out({"scenario", "list", "--touches", "proj", "--scope", "global", "--status", "retired"}) == "(no scenarios)\n");

  auto conn = open_db(fx);
  // Every row survives all three queries — the empty answer above is the
  // filter working, not a missing fixture.
  CHECK(query_rows(conn, "select count(*) from test_scenarios", 1) == "3");
}

TEST_CASE("scenario list --touches an unknown repo REFUSES rather than listing empty") {
  auto const fx = make_fixture("list-touches-badrepo");
  seed(fx);
  add(fx, {"survivor", "--scope", "global"});

  // Falling through to `(no scenarios)` is the silent-filter defect this
  // milestone keeps closing: exit 0, plausible output, wrong answer.
  auto const res = dispatch(fx, {"scenario", "list", "--touches", "nosuchrepo"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: repo 'nosuchrepo' not found\n");
  CHECK(res.out.empty());
}

// ===========================================================================
// scenario link
// ===========================================================================

TEST_CASE("scenario link writes the edge and renders the test_scenario spelling") {
  auto const fx = make_fixture("link");
  seed(fx);
  add(fx, {"s", "--scope", "global"});
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--scope", "global"}).code == 0);

  auto const res = dispatch(fx, {"scenario", "link", "1", "plan:1", "--relationship", "verifies"});
  REQUIRE(res.code == 0);
  // DOUBLE spaces around the bracket group, ASCII arrow, and the subject
  // rendered as `test_scenario` even though the verb is `scenario`.
  CHECK(res.out == "linked test_scenario:1 -> plan:1  [verifies]  (link id: 1)\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|plan|1|verifies");
  // Unlike `scenario add --plan`, THIS path DOES write a `link` audit row —
  // it goes through `engine_entitylink`, which the create path deliberately
  // does not.
  CHECK(audit_rows(conn, "entity_link") == "link|1|<NULL>|<NULL>|<NULL>");
}

TEST_CASE("scenario link --json uses the scenario_id key") {
  auto const fx = make_fixture("link-json");
  seed(fx);
  add(fx, {"s", "--scope", "global"});
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--scope", "global"}).code == 0);

  auto const res = dispatch(fx, {"scenario", "link", "1", "plan:1", "--relationship", "verifies", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains(R"("scenario_id":1)"));
  CHECK(res.out.contains(R"("to_kind":"plan")"));
}

TEST_CASE("scenario link refuses a duplicate, a missing endpoint, and a malformed ref") {
  auto const fx = make_fixture("link-refusals");
  seed(fx);
  add(fx, {"s", "--scope", "global"});
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor", "--scope", "global"}).code == 0);
  REQUIRE(dispatch(fx, {"scenario", "link", "1", "plan:1", "--relationship", "verifies"}).code == 0);

  auto const dup = dispatch(fx, {"scenario", "link", "1", "plan:1", "--relationship", "verifies"});
  CHECK(dup.code == 1);
  CHECK(dup.err == "error: link test_scenario:1 -> plan:1 [verifies] already exists\n");

  auto const missing = dispatch(fx, {"scenario", "link", "1", "plan:999", "--relationship", "verifies"});
  CHECK(missing.code == 1);
  // The message NAMES THE SIDE that was missing, which matters with two
  // refs in play.
  CHECK(missing.err == "error: plan:999 not found\n");

  auto const no_rel = dispatch(fx, {"scenario", "link", "1", "plan:1"});
  CHECK(no_rel.code == 2);
  CHECK(no_rel.err == "error: --relationship is required\n");

  auto const bad_rel = dispatch(fx, {"scenario", "link", "1", "plan:1", "--relationship", "bogus"});
  CHECK(bad_rel.code == 2);
  CHECK(bad_rel.err == "error: unknown relationship 'bogus'\n");

  auto const bad_ref = dispatch(fx, {"scenario", "link", "1", "badref", "--relationship", "verifies"});
  CHECK(bad_ref.code == 2);
  CHECK(bad_ref.err == "error: invalid ref 'badref': expected kind:integer-id\n");

  // Ordering: the SUBJECT id parses FIRST, so a non-integer subject reports
  // the scenario id even with `--relationship` absent.
  auto const bad_id = dispatch(fx, {"scenario", "link", "abc", "plan:1"});
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: scenario id must be an integer, got 'abc'\n");

  auto conn = open_db(fx);
  // After SIX refusals: exactly the ONE edge the successful call made.
  CHECK(edge_rows(conn) == "1|plan|1|verifies");
}
