// @file plan_task_remainder_leaves.t.cpp
// @brief In-process tests for the eight plan/task leaves wired by plan 996,
// task 6187 — the five-leaf `plan step` family (`add`, `list`, `done`,
// `skip`, `link`) and three of the four `task touches` leaves (`add`,
// `list`, `remove`) — plus the `--touches` filter those three unblocked on
// `plan list` and `task list`.
//
// Its own file rather than more of `handlers.t.cpp` (3.8k lines already),
// on the same discipline `annotate_leaves.t.cpp` established:
//
// ## EVERY CASE ASSERTS DATABASE ROWS
//
// Stdout is checked where the bytes are the contract, but no case rests on
// stdout alone. The defect class this milestone keeps finding is a verb
// that exits 0 with oracle-identical output and wrong rows — `capture
// session` wrote NULL columns that way; `list_plans` with no status
// predicate resurrected `done` plans; `annotate sweep --scope` was inert on
// a destructive bulk mutation. So every mutation here is followed by a
// `step_rows` / `edge_rows` / `path_rows` snapshot, and every REFUSAL is
// followed by one too — a refusal that half-wrote is the worse bug, and
// only the after-state distinguishes it.
//
// SQL NULL renders as the literal `<NULL>` in those snapshots and is never
// collapsed to the empty string. `plan_steps.task_id` is genuinely
// nullable and `column_int64` returns 0 for NULL, so a port that read it
// unconditionally would make "unlinked" and "linked to task 0"
// indistinguishable — in the ROWS, not just in the rendering.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, BY A SURVIVOR
//
// The `--touches` cases seed rows on BOTH sides of every predicate and
// assert two things per filter: the returned id set is the proper subset,
// AND the excluded row is still IN THE TABLE afterwards. Asserting only a
// count would report an arithmetic mismatch where the real question is
// whether a read filter quietly became a blast radius.
//
// The three-way scope discrimination in `plan list --touches` is the
// sharpest of these: the same fixture returns {1}, {2} and {4} for
// `--scope repo:other`, `--scope global` and `--scope project:dx`. An
// inert filter returns the same set three times; a uniformly-applied one
// returns {} for two of them. Only the oracle's actual branch asymmetry
// produces three different non-empty answers.
//
// ## ORACLE PROVENANCE
//
// Every expected byte string was captured from `zig/zig-out/bin/planar`
// against a scratch `PLANAR_DB`, read back through `od -c` / a Python
// `repr` rather than through a pipe into `tail`. The whole set was then
// re-derived as a SEQUENCE diff: the same argv script was replayed against
// both binaries and their stdout, stderr, exit codes AND resulting
// `plan_steps` / `entity_links` / `task_touch_paths` / `audit_log` dumps
// compared. The captures that decided a shape:
//
//   $Z plan step add 1 x --after 1   exit 1
//       stderr b'error: ordinal conflict: a step at that position already exists\n'
//       ^ --after is an ORDINAL, not an insert position. It renumbers nothing.
//   $Z plan step list 999            exit 0  stdout b'no steps for plan 999\n'
//       ^ an unknown plan lists EMPTY; it does not refuse.
//   $Z plan step list 1              exit 0, 176 bytes for a three-step plan:
//       b'ord    id          status      body'
//       b'+1     +1          done        first step'
//       b'+5     +3          pending     explicit five  [task:1]'
//       ^ the leading `+` is the ORACLE'S. See `render_step_list_text`.
//   $Z plan step done <terminal>     exit 1  b'error: step 1 is already terminal\n'
//   $Z plan step skip <terminal>     exit 1  b'error: step 2 cannot be skipped (must be pending)\n'
//       ^ ONE engine error, TWO messages, both exit 1.
//   $Z plan step link 3 99           exit 1  b'error: step 3 or task 99 not found\n'
//       ^ a missing step and a missing task report identically.
//   $Z plan step add abc x           exit 2  b"error: plan-id must be an integer, got 'abc'\n"
//       ^ `plan-id`, not `plan id` — this family names the positional
//         verbatim, unlike `plan show` ("plan id must be ..."). Passing
//         "plan-id" as the shared helper's LABEL produced `plan-id id must
//         be ...`, which no exit code could have caught (both are 2).
//   $Z task touches add 1 r          exit 0  b'touches link added: task:1 -> repo:r\n'
//   $Z task touches remove 1 r       exit 0  b'touches link removed: task:1 \xe2\x86\x92 repo:r\n'
//       ^ ASCII `->` on add, U+2192 on remove. Same family, same session.
//   $Z task touches add 1 r --json   b'{"ok":true,...,"path":null}\n'
//       ^ `path` is always PRESENT, null in repo-level mode.

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
                         std::format("planar_rem_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "other", ec);
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
/// The `<NULL>` rendering carries the weight: `plan_steps.task_id` is
/// nullable and `column_int64` yields 0 for NULL, so collapsing the two
/// would let a port that never distinguishes them pass every assertion
/// here AND produce identical stdout.
/// `planar.db`'s `statement` exposes no column count, so each caller
/// states the width of its own projection. Passing the wrong width is
/// caught immediately by the expected string, not silently truncated.
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

/// @brief Every `plan_steps` row as `id|plan|ordinal|body|status|task_id`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto step_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn, "select id, plan_id, ordinal, body, status, task_id from plan_steps order by id", 6);
}

/// @brief Every `touches` edge as `id|from_id|to_id`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto edge_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn,
                    "select id, from_id, to_id from entity_links where from_kind = 'task' and to_kind = 'repo' "
                    "and relationship = 'touches' order by id",
                    3);
}

/// @brief Every `task_touch_paths` row as `id|task|repo|path`.
/// @param conn An open connection to the fixture database.
/// @return The rendered rows, ascending by id.
auto path_rows(planar::db::connection& conn) -> std::string {
  return query_rows(conn, "select id, task_id, repo_id, path from task_touch_paths order by id", 4);
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

/// @brief Bring a fixture up to an initialised database with one plan and
/// one task, both reachable from the fixture's cwd.
/// @param fx The fixture.
void seed_basic(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "P one", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T one", "--scope", "global", "--json"}).code == 0);
}

} // namespace

// ===========================================================================
// plan step add
// ===========================================================================

TEST_CASE("plan step add appends at max(ordinal)+1 and writes pending rows") {
  auto const fx = make_fixture("stepadd");
  seed_basic(fx);

  auto const first = dispatch(fx, {"plan", "step", "add", "1", "first step", "--json"});
  CHECK(first.code == 0);
  CHECK(first.out == R"({"ok":true,"id":1,"plan_id":1,"ordinal":1,"body":"first step","status":"pending",)"
                     R"("task_id":null,"created_at":)" +
                         first.out.substr(first.out.find(R"("created_at":)") + 13));
  auto const second = dispatch(fx, {"plan", "step", "add", "1", "second step", "--json"});
  CHECK(second.code == 0);

  auto conn = open_db(fx);
  // ordinal 1 then 2, both pending, task_id genuinely NULL rather than 0.
  CHECK(step_rows(conn) == "1|1|1|first step|pending|<NULL>;2|1|2|second step|pending|<NULL>");
  CHECK(audit_rows(conn, "plan_step") == "create|1|add step 1 to plan 1|<NULL>|<NULL>;"
                                         "create|2|add step 2 to plan 1|<NULL>|<NULL>");
}

TEST_CASE("plan step add --after sets the ordinal VERBATIM and never renumbers") {
  auto const fx = make_fixture("stepafter");
  seed_basic(fx);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "a", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "b", "--json"}).code == 0);

  // `--after 5` is ordinal FIVE, not "after step 5". Nothing shifts.
  auto const explicit_five = dispatch(fx, {"plan", "step", "add", "1", "e", "--after", "5", "--json"});
  CHECK(explicit_five.code == 0);
  {
    auto conn = open_db(fx);
    CHECK(step_rows(conn) == "1|1|1|a|pending|<NULL>;2|1|2|b|pending|<NULL>;3|1|5|e|pending|<NULL>");
  }

  // `--after 1` collides with the step already at ordinal 1 rather than
  // inserting between 1 and 2 — which is what the flag's NAME suggests.
  auto const collision = dispatch(fx, {"plan", "step", "add", "1", "x", "--after", "1", "--json"});
  CHECK(collision.code == 1);
  CHECK(collision.err == "error: ordinal conflict: a step at that position already exists\n");

  auto conn = open_db(fx);
  // The refusal wrote NOTHING: same three rows, and no fourth audit row.
  CHECK(step_rows(conn) == "1|1|1|a|pending|<NULL>;2|1|2|b|pending|<NULL>;3|1|5|e|pending|<NULL>");
  CHECK(audit_rows(conn, "plan_step") == "create|1|add step 1 to plan 1|<NULL>|<NULL>;"
                                         "create|2|add step 2 to plan 1|<NULL>|<NULL>;"
                                         "create|3|add step 5 to plan 1|<NULL>|<NULL>");
}

TEST_CASE("plan step add refuses an unknown plan and writes no row") {
  auto const fx = make_fixture("stepnoplan");
  seed_basic(fx);

  auto const missing = dispatch(fx, {"plan", "step", "add", "999", "x", "--json"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no plan with id 999\n");

  auto conn = open_db(fx);
  CHECK(step_rows(conn).empty());
  CHECK(audit_rows(conn, "plan_step").empty());
}

TEST_CASE("plan step add rejects a non-integer plan id with the family's own wording") {
  auto const fx = make_fixture("stepbadid");
  seed_basic(fx);

  auto const bad = dispatch(fx, {"plan", "step", "add", "abc", "x", "--json"});
  CHECK(bad.code == 2);
  // `plan-id`, NOT `plan id`. The sibling `plan show` says the latter.
  CHECK(bad.err == "error: plan-id must be an integer, got 'abc'\n");
}

// ===========================================================================
// plan step list
// ===========================================================================

TEST_CASE("plan step list renders the oracle's signed-ordinal table") {
  auto const fx = make_fixture("steplist");
  seed_basic(fx);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "first step", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "second step", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "explicit five", "--after", "5", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "done", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "skip", "2"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "link", "3", "1", "--json"}).code == 0);

  auto const listed = dispatch(fx, {"plan", "step", "list", "1"});
  CHECK(listed.code == 0);
  // Byte-for-byte the oracle's 176-byte capture. The `+` on the two integer
  // columns is the reference's own; see `render_step_list_text`.
  CHECK(listed.out == "ord    id          status      body\n"
                      "+1     +1          done        first step\n"
                      "+2     +2          skipped     second step\n"
                      "+5     +3          pending     explicit five  [task:1]\n");

  // The JSON array's objects carry NO `ok` sentinel; the mutation leaves do.
  auto const as_json = dispatch(fx, {"plan", "step", "list", "1", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.out.starts_with(R"([{"id":1,"plan_id":1,"ordinal":1,"body":"first step","status":"done","task_id":null,)"));
  CHECK(as_json.out.find(R"("ok":)") == std::string::npos);
}

TEST_CASE("plan step list on an unknown plan lists EMPTY at exit 0") {
  auto const fx = make_fixture("steplistnone");
  seed_basic(fx);

  // Deliberately NOT a refusal. The oracle performs no existence check
  // here, and tightening it would change an operator-visible exit code.
  auto const text = dispatch(fx, {"plan", "step", "list", "999"});
  CHECK(text.code == 0);
  CHECK(text.out == "no steps for plan 999\n");

  auto const as_json = dispatch(fx, {"plan", "step", "list", "999", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.out == "[]\n");
}

// ===========================================================================
// plan step done / skip
// ===========================================================================

TEST_CASE("plan step done transitions once and then refuses without rewriting") {
  auto const fx = make_fixture("stepdone");
  seed_basic(fx);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "a", "--json"}).code == 0);

  CHECK(dispatch(fx, {"plan", "step", "done", "1", "--json"}).code == 0);
  {
    auto conn = open_db(fx);
    CHECK(step_rows(conn) == "1|1|1|a|done|<NULL>");
    // The arrow in the summary is U+2192, not `->`.
    CHECK(audit_rows(conn, "plan_step") == "create|1|add step 1 to plan 1|<NULL>|<NULL>;"
                                           "status_change|1|step 1: pending \xe2\x86\x92 done|<NULL>|<NULL>");
  }

  auto const again = dispatch(fx, {"plan", "step", "done", "1", "--json"});
  CHECK(again.code == 1);
  CHECK(again.err == "error: step 1 is already terminal\n");

  auto conn = open_db(fx);
  // Still `done`, and NO second status_change row: a refused transition
  // writes no audit entry.
  CHECK(step_rows(conn) == "1|1|1|a|done|<NULL>");
  CHECK(audit_rows(conn, "plan_step") == "create|1|add step 1 to plan 1|<NULL>|<NULL>;"
                                         "status_change|1|step 1: pending \xe2\x86\x92 done|<NULL>|<NULL>");
}

TEST_CASE("plan step skip refuses with its OWN wording, not done's") {
  auto const fx = make_fixture("stepskip");
  seed_basic(fx);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "a", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "step", "skip", "1", "--json"}).code == 0);

  auto const again = dispatch(fx, {"plan", "step", "skip", "1", "--json"});
  CHECK(again.code == 1);
  // ONE engine error (`invalid_transition`), TWO operator messages. Both
  // exit 1, so nothing but these bytes separates them.
  CHECK(again.err == "error: step 1 cannot be skipped (must be pending)\n");

  auto const as_done = dispatch(fx, {"plan", "step", "done", "1", "--json"});
  CHECK(as_done.code == 1);
  CHECK(as_done.err == "error: step 1 is already terminal\n");

  auto conn = open_db(fx);
  CHECK(step_rows(conn) == "1|1|1|a|skipped|<NULL>");
}

TEST_CASE("an in-progress step refuses skip but accepts done") {
  auto const fx = make_fixture("stepinprog");
  seed_basic(fx);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "a", "--json"}).code == 0);
  {
    // No CLI verb reaches `in-progress`, but the schema's CHECK admits it
    // and the transition matrix treats it asymmetrically — the one status
    // where `done` and `skip` genuinely disagree. Seeded directly.
    auto conn = open_db(fx);
    REQUIRE(conn.execute("update plan_steps set status = 'in-progress' where id = 1").has_value());
  }

  auto const skipped = dispatch(fx, {"plan", "step", "skip", "1", "--json"});
  CHECK(skipped.code == 1);
  CHECK(skipped.err == "error: step 1 cannot be skipped (must be pending)\n");
  {
    auto conn = open_db(fx);
    CHECK(step_rows(conn) == "1|1|1|a|in-progress|<NULL>");
  }

  CHECK(dispatch(fx, {"plan", "step", "done", "1", "--json"}).code == 0);
  auto conn = open_db(fx);
  CHECK(step_rows(conn) == "1|1|1|a|done|<NULL>");
  CHECK(audit_rows(conn, "plan_step").ends_with("status_change|1|step 1: in-progress \xe2\x86\x92 done|<NULL>|<NULL>"));
}

TEST_CASE("plan step done refuses an unknown step") {
  auto const fx = make_fixture("stepdonemissing");
  seed_basic(fx);

  auto const missing = dispatch(fx, {"plan", "step", "done", "99", "--json"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: no step with id 99\n");

  auto const bad_id = dispatch(fx, {"plan", "step", "done", "abc", "--json"});
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: step-id must be an integer, got 'abc'\n");
}

// ===========================================================================
// plan step link
// ===========================================================================

TEST_CASE("plan step link sets task_id and leaves it NULL on refusal") {
  auto const fx = make_fixture("steplink");
  seed_basic(fx);
  REQUIRE(dispatch(fx, {"plan", "step", "add", "1", "a", "--json"}).code == 0);

  // The engine's READ path has to preserve the NULL too, and a row
  // snapshot cannot see that: `step_rows` queries SQLite directly, so it
  // stays `<NULL>` even if `read_row` collapsed the column to 0 on the way
  // out. A break-probe that made `read_row` call `column_int64`
  // unconditionally SURVIVED the row assertions below for exactly that
  // reason. The rendered forms are where the two diverge — `null` vs `0`
  // in JSON, and an absent vs. present `[task:N]` suffix in the table — so
  // both are asserted here, on the same unlinked step.
  auto const before_json = dispatch(fx, {"plan", "step", "list", "1", "--json"});
  CHECK(before_json.code == 0);
  CHECK(before_json.out.contains(R"("task_id":null)"));
  CHECK(!before_json.out.contains(R"("task_id":0)"));
  auto const before_text = dispatch(fx, {"plan", "step", "list", "1"});
  CHECK(before_text.code == 0);
  CHECK(!before_text.out.contains("[task:"));

  auto const missing_task = dispatch(fx, {"plan", "step", "link", "1", "99", "--json"});
  CHECK(missing_task.code == 1);
  CHECK(missing_task.err == "error: step 1 or task 99 not found\n");
  {
    auto conn = open_db(fx);
    // NULL, not 0 — the distinction the `<NULL>` rendering exists for. A
    // port that bound the id before checking would leave a dangling 99.
    CHECK(step_rows(conn) == "1|1|1|a|pending|<NULL>");
    CHECK(audit_rows(conn, "plan_step") == "create|1|add step 1 to plan 1|<NULL>|<NULL>");
  }

  auto const missing_step = dispatch(fx, {"plan", "step", "link", "99", "1", "--json"});
  CHECK(missing_step.code == 1);
  // Identical shape for the other missing endpoint — the oracle does not
  // say WHICH side is absent.
  CHECK(missing_step.err == "error: step 99 or task 1 not found\n");

  auto const linked = dispatch(fx, {"plan", "step", "link", "1", "1", "--json"});
  CHECK(linked.code == 0);
  CHECK(linked.out.contains(R"("task_id":1)"));
  // The same two renderings now carry the id, which is what makes the
  // pre-link assertions above a discrimination rather than a tautology.
  auto const after_json = dispatch(fx, {"plan", "step", "list", "1", "--json"});
  CHECK(after_json.out.contains(R"("task_id":1)"));
  auto const after_text = dispatch(fx, {"plan", "step", "list", "1"});
  CHECK(after_text.out.contains("[task:1]"));

  auto conn = open_db(fx);
  CHECK(step_rows(conn) == "1|1|1|a|pending|1");
  CHECK(audit_rows(conn, "plan_step") == "create|1|add step 1 to plan 1|<NULL>|<NULL>;"
                                         "link|1|link step 1 to task 1|<NULL>|<NULL>");
}

// ===========================================================================
// task touches add / list / remove
// ===========================================================================

namespace {

/// @brief Seed a fixture with an initialised DB, an association, a second
/// registered repo (`other`), and two global tasks.
/// @param fx The fixture.
void seed_touches(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:dx", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:dx", (fx.root / "other").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T one", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "T two", "--scope", "global", "--json"}).code == 0);
}

} // namespace

TEST_CASE("task touches add writes the repo edge and refuses a duplicate") {
  auto const fx = make_fixture("touchadd");
  seed_touches(fx);

  auto const added = dispatch(fx, {"task", "touches", "add", "1", "other"});
  CHECK(added.code == 0);
  // ASCII arrow on add; `remove` uses U+2192. Same family, same session.
  CHECK(added.out == "touches link added: task:1 -> repo:other\n");
  {
    auto conn = open_db(fx);
    CHECK(edge_rows(conn) == "1|1|2");
    CHECK(path_rows(conn).empty());
  }

  // Repo-level mode: a pre-existing edge is a REFUSAL, not a no-op.
  auto const again = dispatch(fx, {"task", "touches", "add", "1", "other", "--json"});
  CHECK(again.code == 1);
  CHECK(again.err == "error: touches link task:1 -> repo:other already exists\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|1|2");
}

TEST_CASE("task touches add --path writes BOTH levels and tolerates the existing edge") {
  auto const fx = make_fixture("touchpath");
  seed_touches(fx);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "other", "--json"}).code == 0);

  // With --path the pre-existing edge is EXPECTED (a path-touch implies the
  // repo-touch), so this succeeds where the repo-level form refused above.
  auto const with_path = dispatch(fx, {"task", "touches", "add", "1", "other", "--path", "src/a.cpp", "--json"});
  CHECK(with_path.code == 0);
  CHECK(with_path.out == "{\"ok\":true,\"task_id\":1,\"repo_id\":2,\"repo_slug\":\"other\",\"path\":\"src/a.cpp\"}\n");
  {
    auto conn = open_db(fx);
    CHECK(edge_rows(conn) == "1|1|2");
    CHECK(path_rows(conn) == "1|1|2|src/a.cpp");
  }

  // Idempotent against unique(task, repo, path) — a silent no-op, not an error.
  CHECK(dispatch(fx, {"task", "touches", "add", "1", "other", "--path", "src/a.cpp", "--json"}).code == 0);
  auto conn = open_db(fx);
  CHECK(path_rows(conn) == "1|1|2|src/a.cpp");
}

TEST_CASE("task touches add refuses an unknown repo slug and an unknown task") {
  auto const fx = make_fixture("touchrefuse");
  seed_touches(fx);

  auto const bad_repo = dispatch(fx, {"task", "touches", "add", "1", "nosuchrepo", "--json"});
  CHECK(bad_repo.code == 1);
  CHECK(bad_repo.err == "error: repo 'nosuchrepo' not found\n");

  auto const bad_task = dispatch(fx, {"task", "touches", "add", "99", "other", "--json"});
  CHECK(bad_task.code == 1);
  // Only the TASK side can be missing here — the repo id came from a
  // successful slug lookup — so the message names the task.
  CHECK(bad_task.err == "error: task:99 not found\n");

  auto conn = open_db(fx);
  CHECK(edge_rows(conn).empty());
  CHECK(path_rows(conn).empty());
}

TEST_CASE("task touches list orders by slug and reports the empty case") {
  auto const fx = make_fixture("touchlist");
  seed_touches(fx);
  // Register a second repo whose slug sorts BEFORE `other`, added SECOND,
  // so slug order and insertion order disagree and the ORDER BY is proven.
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "alpha", ec);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:dx", (fx.root / "alpha").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "other", "--path", "z/last.cpp", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "alpha", "--path", "a/first.cpp", "--json"}).code == 0);

  auto const listed = dispatch(fx, {"task", "touches", "list", "1"});
  CHECK(listed.code == 0);
  CHECK(listed.out == "task:1 touches\n"
                      "  repo: alpha\n"
                      "  repo: other\n"
                      "  path: alpha:a/first.cpp\n"
                      "  path: other:z/last.cpp\n");

  auto const as_json = dispatch(fx, {"task", "touches", "list", "1", "--json"});
  CHECK(as_json.code == 0);
  CHECK(as_json.out == R"({"task_id":1,"repos":["alpha","other"],)"
                       R"("paths":[{"repo":"alpha","path":"a/first.cpp"},{"repo":"other","path":"z/last.cpp"}]})"
                       "\n");

  // Task 2 exists and declares nothing; task 99 does not exist. Both list
  // empty at exit 0 — the oracle performs no existence check here either.
  for (auto const id : {"2", "99"}) {
    auto const empty = dispatch(fx, {"task", "touches", "list", id});
    CHECK(empty.code == 0);
    CHECK(empty.out == std::format("task:{} touches\n  (none declared)\n", id));
  }
}

TEST_CASE("task touches remove --path leaves the repo edge SURVIVING") {
  auto const fx = make_fixture("touchrmpath");
  seed_touches(fx);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "other", "--path", "src/a.cpp", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "other", "--path", "src/b.cpp", "--json"}).code == 0);

  auto const undeclared = dispatch(fx, {"task", "touches", "remove", "1", "other", "--path", "nope.cpp", "--json"});
  CHECK(undeclared.code == 1);
  CHECK(undeclared.err == "error: task:1 has no declared path touch 'nope.cpp' on repo:other\n");
  {
    auto conn = open_db(fx);
    // The refusal removed NOTHING.
    CHECK(path_rows(conn) == "1|1|2|src/a.cpp;2|1|2|src/b.cpp");
  }

  CHECK(dispatch(fx, {"task", "touches", "remove", "1", "other", "--path", "src/a.cpp", "--json"}).code == 0);
  auto conn = open_db(fx);
  // Exactly the named path is gone; the SIBLING path row and the repo edge
  // both SURVIVE. Asserting a count here would not distinguish "removed the
  // right one" from "removed one of them".
  CHECK(path_rows(conn) == "2|1|2|src/b.cpp");
  CHECK(edge_rows(conn) == "1|1|2");
}

TEST_CASE("task touches remove of the repo edge leaves the path rows SURVIVING") {
  auto const fx = make_fixture("touchrmedge");
  seed_touches(fx);
  REQUIRE(dispatch(fx, {"task", "touches", "add", "1", "other", "--path", "src/a.cpp", "--json"}).code == 0);

  auto const removed = dispatch(fx, {"task", "touches", "remove", "1", "other"});
  CHECK(removed.code == 0);
  // U+2192 on remove, ASCII `->` on add.
  CHECK(removed.out == "touches link removed: task:1 \xe2\x86\x92 repo:other\n");
  {
    auto conn = open_db(fx);
    CHECK(edge_rows(conn).empty());
    // DELIBERATELY orphaned. Withdrawing the coarse edge is NOT a way to
    // withdraw a path claim: the eligibility rules read `task_touch_paths`
    // directly, so this row keeps driving them. Documented in the verb's
    // own help text and asserted here so a "tidying" port cannot cascade.
    CHECK(path_rows(conn) == "1|1|2|src/a.cpp");
  }

  auto const again = dispatch(fx, {"task", "touches", "remove", "1", "other", "--json"});
  CHECK(again.code == 1);
  CHECK(again.err == "error: no touches link between task:1 and repo:other\n");
}

TEST_CASE("task touches add warns — never refuses — on a repo in no association") {
  auto const fx = make_fixture("touchorphan");
  seed_touches(fx);
  {
    // A project row with no `project_associations` entry: the stale-slug
    // shape the advisory exists for.
    auto conn = open_db(fx);
    REQUIRE(conn.execute("insert into projects (slug, name, root_path) values ('orphan', 'orphan', '/tmp/orphan')").has_value());
  }

  auto const added = dispatch(fx, {"task", "touches", "add", "1", "orphan"});
  // The write PROCEEDS. A hard refusal would break the polyrepo workflow
  // link verbs exist to serve.
  CHECK(added.code == 0);
  CHECK(added.err.starts_with("warning: repo 'orphan' belongs to no association"));

  auto conn = open_db(fx);
  CHECK(edge_rows(conn) == "1|1|3");
}

TEST_CASE("task touches add suppresses the orphan advisory under --json") {
  auto const fx = make_fixture("touchorphanjson");
  seed_touches(fx);
  {
    auto conn = open_db(fx);
    REQUIRE(conn.execute("insert into projects (slug, name, root_path) values ('orphan', 'orphan', '/tmp/orphan')").has_value());
  }

  auto const added = dispatch(fx, {"task", "touches", "add", "1", "orphan", "--json"});
  CHECK(added.code == 0);
  // A machine consumer gets a clean stream. Oracle-derived: the reference
  // guards the whole advisory behind `if (!args.json)`.
  CHECK(added.err.empty());
}

// ===========================================================================
// plan list --touches / task list --touches
// ===========================================================================

namespace {

/// @brief Seed the fixture used by every `--touches` case.
///
/// Four plans and three tasks spanning the whole predicate space, so each
/// filter below has rows on BOTH sides of it:
///
///   plan 1  repo:other, no edge      — reachable ONLY via the direct branch
///   plan 2  global, edge to other    — reachable ONLY via the touches branch
///   plan 3  global, NO edge          — reachable by NEITHER; the survivor
///   plan 4  project:dx, edge         — touches branch, non-global scope
///
///   task 1  repo:other, no edge      task 2  global, edge      task 3  neither
///
/// Plan edges are inserted directly: `links add` is itself unported, so
/// routing the seed through it would make this fixture depend on a leaf
/// outside this cycle. The task edge goes through the real `task touches
/// add` handler, which IS under test.
/// @param fx The fixture.
void seed_touches_filter(const fixture& fx) {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:dx", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:dx", (fx.root / "other").string(), "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"plan", "create", "Direct repo plan", "--scope", "repo:other", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Global touching plan", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Unrelated plan", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Assoc touching plan", "--scope", "project:dx", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "add", "Direct repo task", "--scope", "repo:other", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Global touching task", "--scope", "global", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Unrelated task", "--scope", "global", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"task", "touches", "add", "2", "other", "--json"}).code == 0);
  auto conn = open_db(fx);
  REQUIRE(conn.execute("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
                       "('plan', 2, 'repo', (select id from projects where slug = 'other'), 'touches'), "
                       "('plan', 4, 'repo', (select id from projects where slug = 'other'), 'touches')")
              .has_value());
}

/// @brief The `id` values in a `--json` listing, comma-joined in order.
/// @param body The listing's stdout.
/// @return e.g. `"1,4"`, or `""` for an empty array.
auto ids_of(std::string_view body) -> std::string {
  std::string    joined;
  constexpr auto key = std::string_view{R"("id":)"};
  std::size_t    at  = 0;
  while ((at = body.find(key, at)) != std::string_view::npos) {
    at += key.size();
    std::size_t end = at;
    while (end < body.size() && (std::isdigit(static_cast<unsigned char>(body[end])) != 0)) {
      ++end;
    }
    if (!joined.empty()) {
      joined += ',';
    }
    joined += body.substr(at, end - at);
    at = end;
  }
  return joined;
}

} // namespace

TEST_CASE("plan list --touches applies the scope predicate to ONE branch only") {
  auto const fx = make_fixture("plantouchscope");
  seed_touches_filter(fx);

  // `--scope repo:other` turns the DIRECT branch on. Plan 1 (repo-scoped,
  // no edge) is returned; plan 2 (global, edged) is EXCLUDED because the
  // touches branch's scope predicate rejects a global row.
  auto const direct = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", "repo:other", "--json"});
  CHECK(direct.code == 0);
  CHECK(ids_of(direct.out) == "1");

  // `--scope global` turns the direct branch OFF wholesale, so plan 1
  // vanishes even though it is the only DIRECTLY repo-scoped plan; plan 2
  // survives through the touches branch. This is the asymmetry: a filter
  // applied uniformly to both branches would return {} here.
  auto const global = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", "global", "--json"});
  CHECK(global.code == 0);
  CHECK(ids_of(global.out) == "2");

  // A third, different non-empty answer. An INERT filter returns the same
  // set three times; a uniform one returns {} twice. Only the real branch
  // asymmetry produces {1}, {2}, {4}.
  auto const assoc = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", "project:dx", "--json"});
  CHECK(assoc.code == 0);
  CHECK(ids_of(assoc.out) == "4");

  auto conn = open_db(fx);
  // Every excluded row SURVIVES. A read filter must never be a blast
  // radius, and a count-only assertion could not tell the two apart.
  CHECK(query_rows(conn, "select id from plans order by id", 1) == "1;2;3;4");
}

TEST_CASE("plan list --touches never returns the unrelated plan") {
  auto const fx = make_fixture("plantouchexcl");
  seed_touches_filter(fx);

  // Plan 3 is global with NO edge — reachable by neither branch. It must
  // be absent from every one of these and present in the table after.
  for (auto const& scope : {"repo:other", "global", "project:dx"}) {
    auto const listed = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", scope, "--json"});
    CHECK(listed.code == 0);
    CHECK(ids_of(listed.out).find('3') == std::string::npos);
  }

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, title from plans where id = 3", 2) == "3|Unrelated plan");
}

TEST_CASE("task list --touches applies the same branch asymmetry") {
  auto const fx = make_fixture("tasktouchscope");
  seed_touches_filter(fx);

  auto const direct = dispatch(fx, {"task", "list", "--touches", "other", "--scope", "repo:other", "--json"});
  CHECK(direct.code == 0);
  CHECK(ids_of(direct.out) == "1");

  auto const global = dispatch(fx, {"task", "list", "--touches", "other", "--scope", "global", "--json"});
  CHECK(global.code == 0);
  // Task 3 is global too, but carries no edge — so this proves the touches
  // predicate excludes, not merely that the scope predicate does.
  CHECK(global.out.find(R"("id":3)") == std::string::npos);
  CHECK(ids_of(global.out) == "2");

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id from tasks order by id", 1) == "1;2;3");
}

TEST_CASE("the touches filter refuses an unknown repo slug rather than listing empty") {
  auto const fx = make_fixture("touchbadslug");
  seed_touches_filter(fx);

  // Falling through to an empty listing is the silent-filter defect this
  // milestone keeps closing: it is indistinguishable from "no matches".
  auto const plans = dispatch(fx, {"plan", "list", "--touches", "nosuchrepo", "--json"});
  CHECK(plans.code == 1);
  CHECK(plans.err == "error: repo 'nosuchrepo' not found\n");

  auto const tasks = dispatch(fx, {"task", "list", "--touches", "nosuchrepo", "--json"});
  CHECK(tasks.code == 1);
  CHECK(tasks.err == "error: repo 'nosuchrepo' not found\n");
}

TEST_CASE("plan list --touches keeps the open-status default on BOTH branches") {
  auto const fx = make_fixture("touchstatus");
  seed_touches_filter(fx);
  // Close one plan on each branch. The empty-status default is the OPEN
  // set, emitted per branch — the omission that resurrected `done` plans
  // in `list_plans` would show up here as a closed plan still listed.
  // `draft -> done` is an illegal hop (oracle: `plan update:
  // IllegalTransition`); the legal route is via `active`.
  for (auto const& id : {"1", "2"}) {
    REQUIRE(dispatch(fx, {"plan", "update", id, "--status", "active", "--json"}).code == 0);
    REQUIRE(dispatch(fx, {"plan", "update", id, "--status", "done", "--json"}).code == 0);
  }

  auto const direct = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", "repo:other", "--json"});
  CHECK(direct.code == 0);
  CHECK(ids_of(direct.out).empty());

  auto const global = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", "global", "--json"});
  CHECK(global.code == 0);
  CHECK(ids_of(global.out).empty());

  // Named explicitly, they come back — which is what proves the default
  // was a STATUS filter and not an unrelated exclusion.
  auto const named = dispatch(fx, {"plan", "list", "--touches", "other", "--scope", "global", "--status", "done", "--json"});
  CHECK(named.code == 0);
  CHECK(ids_of(named.out) == "2");

  auto conn = open_db(fx);
  CHECK(query_rows(conn, "select id, status from plans where id in (1,2) order by id", 2) == "1|done;2|done");
}
