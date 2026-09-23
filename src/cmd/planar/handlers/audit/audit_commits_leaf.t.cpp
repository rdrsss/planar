// @file audit_commits_leaf.t.cpp
// @brief In-process tests for `planar audit commits`, ported by plan 996,
// task 6277.
//
// ## THIS LEAF WAS DEFERRED FOR A MILESTONE ON A CLAIM THAT WAS FALSE
//
// `surface.cpp`, `dispatch.cpp`, `handlers/audit.cppm` and
// `engine/runtime/sessioncommits.cppm` all recorded `audit commits` as
// blocked on the git-walk seam, grouped with `capture commits` and `bench
// harvest`. The oracle handler is 78 lines and spawns no process: it calls
// `session.getById`, `task.show`, `listFiltered` and `writeJsonList`, all
// pure SQL. Walking git is what WRITES `session_commits`; this leaf reads
// it. The dependency was inferred from the leaf's MODULE rather than from
// its own handler, and the four files agreed with each other for two tasks.
//
// ## THE THREE OUTPUT SHAPES DISAGREE ON THE EMPTY CASE, ALL THREE WAYS
//
//     text   -> the HEADER ROW ALONE
//     --json -> []
//     --shas -> zero bytes
//
// No two of them are derivable from the third and a consumer written
// against any one breaks on the others. All three are pinned against a
// task that exists and has no commits — a real state, not an error.
//
// ## `--task` DROPS ROWS `--session` KEEPS, AND THE FIXTURE IS BUILT ON IT
//
// `session_commits` has no `task_id`. The task predicate joins
// `agent_work_claims` through `claim_id`, and the join is INNER, so a
// commit recorded outside any claim window (`claim_id is null`) vanishes
// from a task-filtered listing while remaining in the session-filtered and
// unfiltered ones. Row `bbbb222` below exists to make that observable: the
// three listings return three, three and TWO rows over the same table. A
// fixture whose rows all carried a claim would pass identically against a
// port that ignored the join entirely.
//
// ## THE SORT COLUMN IS NOT THE ONE THE TABLE PRINTS
//
// Rows come back `recorded_at desc, id desc` while the rendered column is
// `committed_at`. The fixture's three rows disagree on exactly this — the
// oldest `committed_at` (2025) is in the middle of the output and the row
// with NO `committed_at` sorts first — so a port that sorted by the visible
// column would produce a plausible, differently-ordered table. Captured
// from `zig/zig-out/bin/planar` in a pinned arena, against these same rows.
//
// ## COMMIT SUBJECTS ARE JSON-ESCAPED; THE `assoc` SUCCESS LITERALS ARE NOT
//
// `writeJson` routes every string through
// `std.json.Stringify.encodeJsonString`, so a subject containing `"` or
// `\` escapes properly. That is the OPPOSITE of the hand-rolled `assoc add`
// / `assoc remove` `--json` literals, which interpolate raw and emit
// invalid JSON for the same input. The two are the same shape of
// hand-assembled object and behave differently, so the fixture carries a
// subject with both characters rather than assuming either way.
//
// ## THE SEEDED ROWS ARE ASSERTED NON-EMPTY BEFORE ANYTHING IS READ
//
// `session_commits` and `agent_work_claims` have no writer this binary can
// reach in-process, so they are INSERTED. An insert that violates a CHECK
// constraint is silently dropped by the seeding path and every assertion
// then pins the empty answer — which has already happened once on this
// milestone (`agent_actions.outcome` accepts only ok|error|aborted|timeout;
// a cycle wrote 'success' and lost both rows). Guard (b) below counts every
// seeded table.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
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

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_acl_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table.
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

/// @brief Run one statement against the fixture database, failing loudly.
/// @param conn The open connection.
/// @param sql The statement.
void exec(planar::db::connection& conn, std::string_view sql) {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// @brief Count rows in one table.
/// @param conn The open connection.
/// @param table The table name.
/// @return The row count.
auto count(planar::db::connection& conn, std::string_view table) -> std::int64_t {
  auto stmt = conn.prepare(std::format("select count(*) from {}", table));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Seed a scope, two tasks, one session, one claim on task 1, and
/// three commits.
///
/// TASK 2 CARRIES NO CLAIM ON PURPOSE. It is the "exists but has no
/// commits" case, and it has to be a real task or the empty-shape
/// assertions would be indistinguishable from the not-found refusal.
///
/// The commit rows are chosen so every discriminating property is
/// observable at once:
///   - `bbbb222` has a NULL `claim_id` (dropped by `--task`, kept by
///     `--session`), a NULL `committed_at` (renders `-`) and a NULL
///     `subject` (renders EMPTY, not `-`).
///   - `cccc333` has the OLDEST `committed_at` but a middling
///     `recorded_at`, so the sort column is distinguishable from the
///     displayed one, and a subject containing both `"` and `\`.
///   - `aaaa111` is the ordinary fully-populated row.
/// @param fx The fixture.
void seed_commits(const fixture& fx) {
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  // ABSOLUTE. `assoc add project:proj .` stores a literal dot no
  // cwd-derivation can match (task 6256) and the plan/task writes below
  // would then land unscoped.
  CHECK(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Commits plan", "--json"}).code == 0);
  // `--body` supplied so the write never reaches the editor path.
  CHECK(dispatch(fx, {"task", "add", "Task one", "--plan", "1", "--body", "one", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "Task two", "--plan", "1", "--body", "two", "--json"}).code == 0);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());

  // GUARD (a): the CLI half actually wrote. Without it the inserts below
  // would install their rows over an empty database and the two tasks the
  // `--task` cases name would not exist.
  CHECK(count(*conn, "tasks") == 2);

  exec(*conn, "delete from session_commits");
  exec(*conn, "delete from agent_work_claims");
  exec(*conn, "delete from sessions");
  exec(*conn, "insert into sessions (id, vendor, task_id, started_at) values (1, 'claude', 1, '2026-02-01T00:00:00.000Z')");
  exec(*conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, status, "
              "vendor, claimed_at, last_heartbeat_at, lease_expires_at) values "
              "(1, 'tok-one', 1, 'task', 1, 'exclusive', 'active', 'claude', "
              "'2026-03-01T00:00:00.000Z', '2026-03-01T00:00:00.000Z', '2026-03-01T01:00:00.000Z')");
  exec(*conn, "insert into session_commits (id, session_id, claim_id, sha, repo_root, branch, subject, author, "
              "committed_at, recorded_at) values "
              "(1, 1, 1, 'aaaa111', '/repo', 'main', 'first subject', 'Ann', '2026-01-01T00:00:00Z', "
              "'2026-08-01T00:00:00.000Z'), "
              "(2, 1, null, 'bbbb222', null, null, null, null, null, '2026-08-02T00:00:00.000Z'), "
              "(3, 1, 1, 'cccc333', '/repo', 'main', 'quote \" and \\ backslash', 'Bob', '2025-01-01T00:00:00Z', "
              "'2026-07-01T00:00:00.000Z')");

  // GUARD (b). A CHECK-violating insert is dropped silently and every
  // assertion below would then pin the empty answer. See this file's
  // header.
  CHECK(count(*conn, "sessions") == 1);
  CHECK(count(*conn, "agent_work_claims") == 1);
  CHECK(count(*conn, "session_commits") == 3);
}

/// @brief The unconditional header row, transcribed from the oracle's
/// format string. It prints even when there are no rows.
constexpr std::string_view k_header =
    "SHA                                       session  claim  committed_at               subject\n";

} // namespace

TEST_CASE("audit commits renders the table newest-RECORDED first, not newest-committed", "[cmd][audit][commits]") {
  auto const fx = make_fixture("table");
  seed_commits(fx);

  auto const got = dispatch(fx, {"audit", "commits"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // Order is 2, 1, 3 by `recorded_at desc`. Read down the committed_at
  // column and it goes null, 2026, 2025 — NOT sorted. That is the point.
  // The `bbbb222` line ends in the gutter's two spaces and nothing else,
  // because a null subject renders EMPTY while a null committed_at renders
  // `-`; the trailing whitespace is part of the contract.
  CHECK(got.out == std::format("{}"
                               "bbbb222                                         1      -  -                          \n"
                               "aaaa111                                         1      1  2026-01-01T00:00:00Z       "
                               "first subject\n"
                               "cccc333                                         1      1  2025-01-01T00:00:00Z       "
                               "quote \" and \\ backslash\n",
                               k_header));
}

TEST_CASE("audit commits --json escapes subjects and spells every absent field null", "[cmd][audit][commits][json]") {
  auto const fx = make_fixture("json");
  seed_commits(fx);

  auto const got = dispatch(fx, {"audit", "commits", "--json"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // Keys are ALWAYS present — `claim_id` and the five optional strings
  // render explicit `null` rather than being omitted — and the subject's
  // `"` and `\` are escaped, unlike `assoc remove --json`'s raw
  // interpolation. Both halves are captured, not inferred.
  CHECK(got.out == R"([{"id":2,"session_id":1,"claim_id":null,"sha":"bbbb222","repo_root":null,"branch":null,"subject":null,)"
                   R"("author":null,"committed_at":null,"recorded_at":"2026-08-02T00:00:00.000Z"},)"
                   R"({"id":1,"session_id":1,"claim_id":1,"sha":"aaaa111","repo_root":"/repo","branch":"main",)"
                   R"("subject":"first subject","author":"Ann","committed_at":"2026-01-01T00:00:00Z",)"
                   R"("recorded_at":"2026-08-01T00:00:00.000Z"},)"
                   R"({"id":3,"session_id":1,"claim_id":1,"sha":"cccc333","repo_root":"/repo","branch":"main",)"
                   R"("subject":"quote \" and \\ backslash","author":"Bob","committed_at":"2025-01-01T00:00:00Z",)"
                   R"("recorded_at":"2026-07-01T00:00:00.000Z"}])"
                   "\n");
}

TEST_CASE("audit commits --shas prints bare shas in the same order", "[cmd][audit][commits][shas]") {
  auto const fx = make_fixture("shas");
  seed_commits(fx);

  auto const got = dispatch(fx, {"audit", "commits", "--shas"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // No header, no padding, no table — and the SAME order as the table
  // arm, which is what makes this a rendering difference rather than a
  // second query.
  CHECK(got.out == "bbbb222\naaaa111\ncccc333\n");
}

TEST_CASE("audit commits --task drops NULL-claim rows that --session keeps", "[cmd][audit][commits][filter]") {
  auto const fx = make_fixture("filter");
  seed_commits(fx);

  // Unfiltered: all three. Without this the two filtered assertions would
  // be equally satisfied by a predicate matching nothing.
  auto const all = dispatch(fx, {"audit", "commits", "--shas"});
  CHECK(all.code == 0);
  CHECK(all.out == "bbbb222\naaaa111\ncccc333\n");

  // Session-filtered: still all three. Every row belongs to session 1, so
  // this arm must NOT narrow — it proves the session term is a predicate
  // and not an accidental join.
  auto const by_session = dispatch(fx, {"audit", "commits", "--session", "1", "--shas"});
  CHECK(by_session.code == 0);
  CHECK(by_session.out == "bbbb222\naaaa111\ncccc333\n");

  // Task-filtered: TWO. `bbbb222` has a null `claim_id` and the join is
  // inner. Same table, same session, one fewer row.
  auto const by_task = dispatch(fx, {"audit", "commits", "--task", "1", "--shas"});
  CHECK(by_task.code == 0);
  CHECK(by_task.out == "aaaa111\ncccc333\n");

  // Both together behave as the task arm — the two predicates AND.
  auto const both = dispatch(fx, {"audit", "commits", "--session", "1", "--task", "1", "--shas"});
  CHECK(both.code == 0);
  CHECK(both.out == "aaaa111\ncccc333\n");
}

TEST_CASE("audit commits on a task with no commits answers all three empty shapes", "[cmd][audit][commits][filter]") {
  auto const fx = make_fixture("emptytask");
  seed_commits(fx);

  // Task 2 EXISTS and carries no claim. This is a listing, not a refusal —
  // which is why it has to be a real task id.
  auto const text = dispatch(fx, {"audit", "commits", "--task", "2"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // The HEADER ALONE. Not `(no commits)`, not zero bytes.
  CHECK(text.out == k_header);

  auto const json = dispatch(fx, {"audit", "commits", "--task", "2", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "[]\n");

  // ZERO BYTES — the only shape of the three that prints nothing, and the
  // reason the other two are pinned beside it.
  auto const shas = dispatch(fx, {"audit", "commits", "--task", "2", "--shas"});
  CHECK(shas.code == 0);
  CHECK(shas.out.empty());
}

TEST_CASE("audit commits refuses --json with --shas BEFORE resolving any id", "[cmd][audit][commits][refusal]") {
  auto const fx = make_fixture("combine");
  seed_commits(fx);

  auto const combined = dispatch(fx, {"audit", "commits", "--json", "--shas"});
  CHECK(combined.code == 2);
  CHECK(combined.out == planar::cmd::testsupport::json_error_envelope_line("audit commits", "invalid_input"));
  CHECK(combined.err == "error: cannot combine --json with --shas\n");

  // ORDERING. With a nonexistent session ALSO named, the combination still
  // wins — the flag check runs before the lookups. A port that validated
  // ids first would answer `session 99 not found` here and pass the case
  // above.
  auto const with_bad_id = dispatch(fx, {"audit", "commits", "--json", "--shas", "--session", "99"});
  CHECK(with_bad_id.code == 2);
  CHECK(with_bad_id.err == "error: cannot combine --json with --shas\n");
}

TEST_CASE("audit commits refuses an unknown --session or --task, session first", "[cmd][audit][commits][refusal]") {
  auto const fx = make_fixture("missing");
  seed_commits(fx);

  // Exit 1, not 0-with-nothing. Contrast the sibling `audit trail 99`,
  // which succeeds with an empty trail for a nonexistent entity.
  auto const session = dispatch(fx, {"audit", "commits", "--session", "99"});
  CHECK(session.code == 1);
  CHECK(session.out.empty());
  CHECK(session.err == "error: session 99 not found\n");

  auto const task = dispatch(fx, {"audit", "commits", "--task", "99"});
  CHECK(task.code == 1);
  CHECK(task.out.empty());
  CHECK(task.err == "error: task 99 not found\n");

  // BOTH bad: the SESSION is reported. The lookups run in flag-declaration
  // order and the first failure wins.
  auto const both = dispatch(fx, {"audit", "commits", "--session", "99", "--task", "99"});
  CHECK(both.code == 1);
  CHECK(both.err == "error: session 99 not found\n");

  // And the good ids really do succeed, so the three refusals above are
  // evidence about the lookups rather than about the verb being broken.
  auto const good = dispatch(fx, {"audit", "commits", "--session", "1", "--task", "1", "--shas"});
  CHECK(good.code == 0);
  CHECK(good.out == "aaaa111\ncccc333\n");
}
