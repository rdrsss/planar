// @file audit_trail_leaf.t.cpp
// @brief In-process tests for `planar audit trail` (plan 996, task 6262).
//
// Its own file rather than an extension of search_health_audit_leaves.t.cpp:
// that file exists to hold three families' CROSS-family contradictions side
// by side, and this leaf's interesting content is INTERNAL — two forms that
// share a name, two JSON null conventions inside one payload, and a `--grep`
// that changes which query runs. The one cross-family contradiction that
// does belong next to `audit session` is asserted here anyway, and named
// below, because both halves of it are `audit` leaves.
//
// ## EVERY EXPECTATION HERE WAS CAPTURED FROM THE ORACLE, NOT DERIVED
//
// The fixture below is pinned hard enough that `zig/zig-out/bin/planar` and
// `build/debug/bin/planar` produce BYTE-IDENTICAL output on it — every
// timestamp, id and token is a fixed literal, so there is nothing to
// normalise away. Twelve invocations were diffed with no normalisation at
// all before these strings were written down, and the strings are the
// ORACLE's side of that diff.
//
// ## `audit trail 99` SUCCEEDS WHERE `audit session 99` REFUSES
//
// Same family, same shape of bad id, opposite answers:
//
//     audit trail 99   -> exit 0, `audit trail for task:99  (0 entries)`
//     audit session 99 -> exit 1, `session 99 not found`
//
// An entity with no history is an ANSWER; a session id naming no row is
// not. Both are asserted in one case so a port cannot quietly make them
// agree. (The `audit session` half is also pinned in
// search_health_audit_leaves.t.cpp; the duplication is deliberate — the
// contradiction has to be visible from the side that is easy to get
// wrong.)
//
// ## `--grep` SELECTS A DIFFERENT QUERY, IT DOES NOT FILTER THIS ONE
//
// Without it the leaf calls `for_entity_with_links`, which widens by one
// `entity_links` hop. With it the leaf calls `for_entity_grep`, which does
// NOT widen. So `--grep ''` — a pattern that excludes nothing, since it
// becomes `like '%%'` — still DROPS the linked decision's row, and drops
// the entity's own `status_change` row too because that row's `summary` is
// NULL and `null like x` is NULL rather than true. Three rows become one
// for two independent reasons, neither of them "the pattern did not
// match". Asserted against the three-row baseline in the same case, so the
// drop is visible rather than merely stated.
//
// ## ONE PAYLOAD, TWO NULL CONVENTIONS, AND ONE RAW SPLICE
//
//   entries[] / agent_activity  OMIT unset optionals entirely.
//   commits[]                   emit them as explicit `null`.
//   sync_events[].evidence      is the `context_json` column SPLICED RAW,
//                               so it is a JSON object, not an escaped
//                               string.
//
// All three are in the same document. The `evidence` one is the dangerous
// one: escaping it produces valid JSON of the wrong shape, which a
// "parses as JSON" assertion would accept. The full payload is asserted.
//
// ## HOME / DB SAFETY
//
// Every fixture builds an explicit environment map rooted at its own
// scratch directory — `PLANAR_DB`, `PLANAR_HOME`, `PLANAR_LOCAL_HOME` and
// `HOME` all point inside it, and nothing here reads the process
// environment. This leaf opens SQLite on every path that is not an argv
// refusal, so the pinned `PLANAR_DB` is load-bearing.
//
// ## FIXTURE NON-EMPTINESS IS ASSERTED TWICE, AND THE SECOND CHECK EARNED
// ## ITS PLACE
//
// `audit trail`'s entire output IS a result set, in six independent
// sections, so a fixture that silently stored nothing makes every case
// below pass against a short answer that looks plausible. `seed_trail`
// therefore checks (a) that the CLI seeding wrote real `audit_log` and
// `entity_links` rows before the pin rewrites them, and (b) that every one
// of the seven tables the renderer reads is non-empty AFTER the rewrite.
//
// Check (b) is not defensive padding. The first draft of the pinned
// fixture wrote `outcome = 'success'` into `agent_actions`, which violates
// that column's CHECK constraint (`ok | error | aborted | timeout`). Both
// rows were rejected, the "Agent activity" actions list rendered empty,
// and the corresponding expectations would have been written down against
// an answer with no actions in it — passing forever, pinning nothing.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.external;
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

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Inside `root`; never the operator's.
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_atl_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  auto vars   = fx.vars;
  vars["PWD"] = (fx.root / "proj").string();

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run one statement against the fixture database, failing loudly.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// @brief Count rows in one table.
auto count(planar::db::connection& conn, std::string_view table) -> std::int64_t {
  auto stmt = conn.prepare(std::format("select count(*) from {}", table));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Seed the scope, plan, task, decision and entity link through the
/// CLI, then PIN every value the renderer reads.
///
/// The two halves are both necessary. The CLI half exercises the real write
/// paths and proves the scope resolves (an `assoc add` given `.` stores a
/// literal dot no cwd-derivation can match, so the path below is ABSOLUTE).
/// The pin half replaces the timestamps, ids and tokens those writes
/// produced with fixed literals, because the renderer's output embeds all
/// three and an exact-payload assertion cannot survive a clock.
///
/// Rows for `external_links`, `sync_events`, `session_commits`,
/// `agent_work_claims` and `agent_actions` are INSERTED rather than pinned:
/// the top-level `link` verb is unported, and the other four tables have no
/// writer this binary can reach in-process at all.
void seed_trail(const fixture& fx) {
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Trail plan", "--summary", "trail summary", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "Trail task one", "--plan", "1", "--body", "trail task body", "--json"}).code == 0);
  CHECK(dispatch(fx, {"decision", "add", "Trail decision", "--body", "trail decision body", "--json"}).code == 0);
  CHECK(dispatch(fx, {"links", "add", "task:1", "decision:1", "--relationship", "cites", "--json"}).code == 0);
  // `ext register` moved to `planar-ext` at plan 996, task 6419; seeded
  // directly through the engine here, same as `sync.t.cpp`'s `rig` fixture.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::engine::external::system::register_github(*conn, {.slug = "gh", .project = "acme/widgets"}).has_value());

  // GUARD (a): the CLI seeding actually wrote something. Without this the
  // pin below would happily install its rows over an empty database and the
  // scope/link plumbing would never be exercised.
  CHECK(count(*conn, "audit_log") > 0);
  CHECK(count(*conn, "entity_links") == 1);

  exec(*conn, "delete from audit_log");
  exec(*conn, "insert into audit_log (id, verb, entity_kind, entity_id, actor, scope, summary, recorded_at) values "
              "(1, 'create', 'task', 1, null, null, 'create task ''Trail task one''', '2026-01-01T00:00:00.000Z'), "
              "(2, 'status_change', 'task', 1, null, null, null, '2026-01-02T00:00:00.000Z'), "
              "(3, 'create', 'decision', 1, 'ada', 'project:proj', 'create decision ''Trail decision''', "
              "'2026-01-03T00:00:00.000Z'), "
              "(4, 'create', 'plan', 1, null, null, 'create plan ''Trail plan''', '2026-01-04T00:00:00.000Z')");

  exec(*conn, "delete from agent_actions");
  exec(*conn, "delete from session_commits");
  exec(*conn, "delete from agent_work_claims");
  exec(*conn, "delete from sessions");
  // Session 1 is BOUND to the task; session 2 is not, and is reachable only
  // through its claim. Both arms of `load_sessions_for_entity`'s task query
  // therefore contribute, and dropping either one is visible.
  exec(*conn, "insert into sessions (id, vendor, task_id, started_at, summary) values "
              "(1, 'claude', 1, '2026-02-01T00:00:00.000Z', 'the bound one'), "
              "(2, 'codex', null, '2026-02-02T00:00:00.000Z', null)");

  // Claim 1 is ACTIVE with a role and a long token; claim 2 is RELEASED
  // with no role and a token SHORTER than the eight characters the renderer
  // truncates to. Every optional is present on one and absent on the other.
  exec(*conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, claim_scope, status, "
              "vendor, role, claimed_at, last_heartbeat_at, lease_expires_at, released_at, release_reason) values "
              "(1, 'abcdef0123456789abcdef0123456789', 1, 'task', 1, 'exclusive', 'active', 'claude', 'coder', "
              "'2026-03-01T00:00:00.000Z', '2026-03-01T00:00:00.000Z', '2026-03-01T01:00:00.000Z', null, null), "
              "(2, 'short', 2, 'task', 1, 'exclusive', 'completed', 'codex', null, "
              "'2026-03-02T00:00:00.000Z', '2026-03-02T00:00:00.000Z', '2026-03-02T01:00:00.000Z', "
              "'2026-03-05T00:00:00.000Z', 'done here')");

  // Action 1 is OPEN (renders `outcome=(in-flight)`); action 2 is CLOSED.
  // Action 2 also ENDED after action 1 STARTED, which is what makes the
  // `coalesce(ended_at, started_at) desc` sort distinguishable from a
  // `started_at desc` one.
  exec(*conn, "insert into agent_actions (id, session_id, claim_id, action_kind, entity_kind, entity_id, vendor, "
              "started_at, ended_at, outcome, summary) values "
              "(1, 1, 1, 'tool_call', 'task', 1, 'claude', '2026-04-01T00:00:00.000Z', null, null, null), "
              "(2, 1, 1, 'coder', 'task', 1, 'claude', '2026-04-02T00:00:00.000Z', '2026-04-09T00:00:00.000Z', "
              "'ok', 'did the thing')");

  exec(*conn, "insert into session_commits (id, session_id, claim_id, sha, repo_root, branch, subject, author, "
              "committed_at, recorded_at) values "
              "(1, 1, 1, 'aaaa111', '/repo', 'main', 'first subject', 'Ada', '2026-05-01T00:00:00.000Z', "
              "'2026-05-01T00:00:01.000Z'), "
              "(2, 1, null, 'bbbb222', null, null, null, null, null, '2026-05-02T00:00:00.000Z')");

  exec(*conn, "insert into external_links (id, entity_kind, entity_id, system_id, external_id, link_role, "
              "sync_direction, created_at) values (1, 'task', 1, 1, '42', 'mirror', 'two-way', "
              "'2026-07-01T00:00:00.000Z')");

  exec(*conn, "delete from sync_events");
  exec(*conn, "insert into sync_events (id, link_id, direction, outcome, fields_changed, detail, context_json, at) "
              "values (1, 1, 'push', 'ok', 'title,status', 'pushed cleanly', '{\"token\":\"abc\"}', "
              "'2026-06-01T00:00:00.000Z'), "
              "(2, 1, 'pull', 'conflict', null, null, null, '2026-06-02T00:00:00.000Z')");

  // GUARD (b). Every table the renderer reads must have rows, or a whole
  // section renders nothing and its expectations pin nothing. See this
  // file's header for the CHECK-constraint violation that made this
  // necessary rather than merely tidy.
  for (auto const* table :
       {"audit_log", "sessions", "agent_work_claims", "agent_actions", "session_commits", "sync_events", "external_links"}) {
    INFO(table);
    CHECK(count(*conn, table) > 0);
  }
}

/// @brief The `commits:` + `Agent activity:` tail both forms append.
///
/// Written once because it is literally the same bytes in five of the cases
/// below; a second copy would let one drift.
constexpr std::string_view k_fold_in_tail = "\ncommits:\n"
                                            "  2026-05-02T00:00:00.000Z  bbbb222  (no subject)\n"
                                            "  2026-05-01T00:00:00.000Z  aaaa111  first subject\n"
                                            "\nAgent activity:\n"
                                            "  actions (2):\n"
                                            "    [2026-04-09T00:00:00.000Z]  coder         claude    outcome=ok\n"
                                            "    [2026-04-01T00:00:00.000Z]  tool_call     claude    outcome=(in-flight)\n"
                                            "  claims (2):\n"
                                            "    [2026-03-05T00:00:00.000Z]  completed   codex     token:short…\n"
                                            "    [2026-03-01T00:00:00.000Z]  active      claude    token:abcdef01…\n";

} // namespace

TEST_CASE("audit trail widens by one entity_links hop, in both directions", "[cmd][audit][trail]") {
  auto const fx = make_fixture("hop");
  seed_trail(fx);

  // From the TASK: its own two rows plus the linked decision's one.
  auto const from_task = dispatch(fx, {"audit", "trail", "1"});
  CHECK(from_task.code == 0);
  CHECK(from_task.err.empty());
  CHECK(from_task.out == std::string{"audit trail for task:1  (3 entries)\n"
                                     "  [2026-01-01T00:00:00.000Z]  create          task:1  — create task 'Trail task one'\n"
                                     "  [2026-01-02T00:00:00.000Z]  status_change   task:1\n"
                                     "  [2026-01-03T00:00:00.000Z]  create          decision:1  — create decision 'Trail "
                                     "decision'\n"} +
                             std::string{k_fold_in_tail});

  // From the DECISION, over the SAME edge read backwards: the same three
  // rows. The edge is stored `task -> decision`, so this arm is the one a
  // single-direction implementation loses, and it would still return a
  // plausible one-row answer rather than failing.
  auto const from_decision = dispatch(fx, {"audit", "trail", "1", "--kind", "decision"});
  CHECK(from_decision.code == 0);
  CHECK(from_decision.out == "audit trail for decision:1  (3 entries)\n"
                             "  [2026-01-01T00:00:00.000Z]  create          task:1  — create task 'Trail task one'\n"
                             "  [2026-01-02T00:00:00.000Z]  status_change   task:1\n"
                             "  [2026-01-03T00:00:00.000Z]  create          decision:1  — create decision 'Trail decision'\n");

  // The PLAN has no edge at all, so it sees only its own row — which is
  // what makes the two answers above evidence of traversal rather than of
  // an unfiltered query.
  auto const from_plan = dispatch(fx, {"audit", "trail", "1", "--kind", "plan"});
  CHECK(from_plan.code == 0);
  CHECK(from_plan.out == "audit trail for plan:1  (1 entries)\n"
                         "  [2026-01-04T00:00:00.000Z]  create          plan:1  — create plan 'Trail plan'\n");
}

TEST_CASE("audit trail --grep switches to the NON-widening query", "[cmd][audit][trail][grep]") {
  auto const fx = make_fixture("grep");
  seed_trail(fx);

  // ONE case on purpose: the baseline and the grep answer only mean
  // something next to each other.
  auto const baseline = dispatch(fx, {"audit", "trail", "1"});
  CHECK(baseline.out.starts_with("audit trail for task:1  (3 entries)\n"));

  // A pattern that excludes NOTHING — `like '%%'` — still returns one row
  // of the three. Two independent losses: the linked decision's row is gone
  // because this query does not traverse, and `status_change` is gone
  // because its summary is NULL.
  auto const empty_pattern = dispatch(fx, {"audit", "trail", "1", "--grep", ""});
  CHECK(empty_pattern.code == 0);
  CHECK(empty_pattern.out == std::string{"audit trail for task:1  (1 entries)\n"
                                         "  [2026-01-01T00:00:00.000Z]  create          task:1  — create task 'Trail task "
                                         "one'\n"} +
                                 std::string{k_fold_in_tail});

  // The pattern is UNESCAPED, so `%` and `_` are wildcards, and matching is
  // case-insensitive for ASCII. Both are properties an implementer would
  // naturally "fix".
  CHECK(dispatch(fx, {"audit", "trail", "1", "--grep", "cre%one"}).out.starts_with("audit trail for task:1  (1 entries)\n"));
  CHECK(dispatch(fx, {"audit", "trail", "1", "--grep", "creat_"}).out.starts_with("audit trail for task:1  (1 entries)\n"));
  CHECK(dispatch(fx, {"audit", "trail", "1", "--grep", "CREATE"}).out.starts_with("audit trail for task:1  (1 entries)\n"));

  // And a pattern that genuinely matches nothing is the empty ANSWER, not a
  // refusal.
  auto const miss = dispatch(fx, {"audit", "trail", "1", "--grep", "zzznope"});
  CHECK(miss.code == 0);
  CHECK(miss.out.starts_with("audit trail for task:1  (0 entries)\n  (no audit_log entries)\n"));
}

TEST_CASE("audit trail --json carries all three of its conventions at once", "[cmd][audit][trail][json]") {
  auto const fx = make_fixture("json");
  seed_trail(fx);

  auto const json = dispatch(fx, {"audit", "trail", "1", "--json"});
  CHECK(json.code == 0);
  CHECK(json.err.empty());
  // Asserted whole rather than by probe, because the interesting content is
  // the INTERLEAVING: `entries[]` omits `actor`/`scope` on rows 1-2 and
  // carries them on row 3; `commits[]` spells every unset column `null`;
  // `agent_activity.actions[1]` omits `ended_at`/`outcome`/`summary` where
  // `actions[0]` has them. A payload built to satisfy any one convention
  // fails on the other two.
  CHECK(json.out == "{\"entity_kind\":\"task\",\"entity_id\":1,\"entries\":["
                    "{\"id\":1,\"verb\":\"create\",\"entity_kind\":\"task\",\"entity_id\":1,"
                    "\"recorded_at\":\"2026-01-01T00:00:00.000Z\",\"summary\":\"create task 'Trail task one'\"},"
                    "{\"id\":2,\"verb\":\"status_change\",\"entity_kind\":\"task\",\"entity_id\":1,"
                    "\"recorded_at\":\"2026-01-02T00:00:00.000Z\"},"
                    "{\"id\":3,\"verb\":\"create\",\"entity_kind\":\"decision\",\"entity_id\":1,"
                    "\"recorded_at\":\"2026-01-03T00:00:00.000Z\",\"actor\":\"ada\",\"scope\":\"project:proj\","
                    "\"summary\":\"create decision 'Trail decision'\"}],"
                    "\"commits\":["
                    "{\"id\":2,\"session_id\":1,\"claim_id\":null,\"sha\":\"bbbb222\",\"repo_root\":null,\"branch\":null,"
                    "\"subject\":null,\"author\":null,\"committed_at\":null,\"recorded_at\":\"2026-05-02T00:00:00.000Z\"},"
                    "{\"id\":1,\"session_id\":1,\"claim_id\":1,\"sha\":\"aaaa111\",\"repo_root\":\"/repo\",\"branch\":\"main\","
                    "\"subject\":\"first subject\",\"author\":\"Ada\",\"committed_at\":\"2026-05-01T00:00:00.000Z\","
                    "\"recorded_at\":\"2026-05-01T00:00:01.000Z\"}],"
                    "\"agent_activity\":{\"actions\":["
                    "{\"id\":2,\"session_id\":1,\"action_kind\":\"coder\",\"vendor\":\"claude\","
                    "\"started_at\":\"2026-04-02T00:00:00.000Z\",\"ended_at\":\"2026-04-09T00:00:00.000Z\",\"outcome\":\"ok\","
                    "\"summary\":\"did the thing\"},"
                    "{\"id\":1,\"session_id\":1,\"action_kind\":\"tool_call\",\"vendor\":\"claude\","
                    "\"started_at\":\"2026-04-01T00:00:00.000Z\"}],"
                    "\"claims\":["
                    "{\"id\":2,\"claim_token\":\"short\",\"status\":\"completed\",\"vendor\":\"codex\","
                    "\"claimed_at\":\"2026-03-02T00:00:00.000Z\",\"released_at\":\"2026-03-05T00:00:00.000Z\","
                    "\"release_reason\":\"done here\"},"
                    "{\"id\":1,\"claim_token\":\"abcdef0123456789abcdef0123456789\",\"status\":\"active\",\"vendor\":\"claude\","
                    "\"role\":\"coder\",\"claimed_at\":\"2026-03-01T00:00:00.000Z\"}]}}\n");
}

TEST_CASE("audit trail --link renders the other verb entirely", "[cmd][audit][trail][link]") {
  auto const fx = make_fixture("link");
  seed_trail(fx);

  auto const text = dispatch(fx, {"audit", "trail", "--link", "1"});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // Note the decisions line: `decided_at` is NULL and was coalesced to the
  // empty string, so the line opens with FOUR spaces and no timestamp.
  // Note also that the sync-event line prints `fields_changed` and NOT
  // `detail`, which appears only in `--json`.
  CHECK(text.out == std::string{"audit trail for link 1  (task:1 ↔ gh:42)\n"
                                "\nsessions:\n"
                                "  2026-02-02T00:00:00.000Z  codex  session:2  \"(no summary)\"\n"
                                "  2026-02-01T00:00:00.000Z  claude  session:1  \"the bound one\"\n"
                                "\ndecisions:\n"
                                "    \"Trail decision\"  [proposed]\n"
                                "\nsync events:\n"
                                "  2026-06-01T00:00:00.000Z  push   ok          title,status\n"
                                "    evidence: {\"token\":\"abc\"}\n"
                                "  2026-06-02T00:00:00.000Z  pull   conflict    (no fields)\n"} +
                        std::string{k_fold_in_tail});

  // Session 2 is in that list ONLY through its claim — it has no `task_id`.
  // Dropping the claim arm of the sessions query loses it and still prints
  // a plausible one-session section.
  CHECK(text.out.contains("session:2"));

  auto const json = dispatch(fx, {"audit", "trail", "--link", "1", "--json"});
  CHECK(json.code == 0);
  // `"evidence":{"token":"abc"}` — an OBJECT. Escaping the column would
  // give `"evidence":"{\"token\":\"abc\"}"`, still valid JSON.
  CHECK(json.out.contains("\"evidence\":{\"token\":\"abc\"}"));
  CHECK_FALSE(json.out.contains("\\\"token\\\""));
  CHECK(json.out == "{\"link_id\":1,\"entity_kind\":\"task\",\"entity_id\":1,\"external_id\":\"42\",\"system_slug\":\"gh\","
                    "\"sessions\":["
                    "{\"id\":2,\"vendor\":\"codex\",\"started_at\":\"2026-02-02T00:00:00.000Z\"},"
                    "{\"id\":1,\"vendor\":\"claude\",\"started_at\":\"2026-02-01T00:00:00.000Z\",\"summary\":\"the bound one\"}],"
                    "\"decisions\":[{\"id\":1,\"title\":\"Trail decision\",\"status\":\"proposed\"}],"
                    "\"sync_events\":["
                    "{\"id\":1,\"direction\":\"push\",\"outcome\":\"ok\",\"fields_changed\":\"title,status\","
                    "\"detail\":\"pushed cleanly\",\"evidence\":{\"token\":\"abc\"},\"at\":\"2026-06-01T00:00:00.000Z\"},"
                    "{\"id\":2,\"direction\":\"pull\",\"outcome\":\"conflict\",\"at\":\"2026-06-02T00:00:00.000Z\"}],"
                    "\"commits\":["
                    "{\"id\":2,\"session_id\":1,\"claim_id\":null,\"sha\":\"bbbb222\",\"repo_root\":null,\"branch\":null,"
                    "\"subject\":null,\"author\":null,\"committed_at\":null,\"recorded_at\":\"2026-05-02T00:00:00.000Z\"},"
                    "{\"id\":1,\"session_id\":1,\"claim_id\":1,\"sha\":\"aaaa111\",\"repo_root\":\"/repo\",\"branch\":\"main\","
                    "\"subject\":\"first subject\",\"author\":\"Ada\",\"committed_at\":\"2026-05-01T00:00:00.000Z\","
                    "\"recorded_at\":\"2026-05-01T00:00:01.000Z\"}],"
                    "\"agent_activity\":{\"actions\":["
                    "{\"id\":2,\"session_id\":1,\"action_kind\":\"coder\",\"vendor\":\"claude\","
                    "\"started_at\":\"2026-04-02T00:00:00.000Z\",\"ended_at\":\"2026-04-09T00:00:00.000Z\",\"outcome\":\"ok\","
                    "\"summary\":\"did the thing\"},"
                    "{\"id\":1,\"session_id\":1,\"action_kind\":\"tool_call\",\"vendor\":\"claude\","
                    "\"started_at\":\"2026-04-01T00:00:00.000Z\"}],"
                    "\"claims\":["
                    "{\"id\":2,\"claim_token\":\"short\",\"status\":\"completed\",\"vendor\":\"codex\","
                    "\"claimed_at\":\"2026-03-02T00:00:00.000Z\",\"released_at\":\"2026-03-05T00:00:00.000Z\","
                    "\"release_reason\":\"done here\"},"
                    "{\"id\":1,\"claim_token\":\"abcdef0123456789abcdef0123456789\",\"status\":\"active\",\"vendor\":\"claude\","
                    "\"role\":\"coder\",\"claimed_at\":\"2026-03-01T00:00:00.000Z\"}]}}\n");
}

// TITLE MUST NOT START WITH `--`. ctest runs each case by passing its name
// as the filter argument, and Catch2 parses a leading `--link` as an option
// (`Unrecognised token: --link`, exit non-zero) before it ever looks for a
// test. The case then fails under `ctest` while passing when selected by
// tag, which reads as a flaky test rather than a naming bug. Observed here.
TEST_CASE("audit trail --link WINS over the positional rather than refusing", "[cmd][audit][trail][link]") {
  auto const fx = make_fixture("linkwins");
  seed_trail(fx);

  // The natural port reads the positional first and would answer the ENTITY
  // form here. The oracle answers the LINK form and drops the positional on
  // the floor — no refusal, no merge.
  auto const both = dispatch(fx, {"audit", "trail", "1", "--link", "1"});
  CHECK(both.code == 0);
  CHECK(both.out.starts_with("audit trail for link 1  ("));

  // And it wins even when the positional is one the entity form would have
  // REFUSED, which is the sharpest form of "checked first".
  auto const both_bad = dispatch(fx, {"audit", "trail", "not-an-id", "--link", "1"});
  CHECK(both_bad.code == 0);
  CHECK(both_bad.out.starts_with("audit trail for link 1  ("));
}

TEST_CASE("audit trail SUCCEEDS on an unknown entity where audit session REFUSES", "[cmd][audit][trail][notfound]") {
  auto const fx = make_fixture("unknown");
  seed_trail(fx);

  // ONE case, both halves. Same family, same shape of bad id, opposite
  // answers; split apart, "harmonising" them would leave one green.
  auto const trail = dispatch(fx, {"audit", "trail", "99"});
  CHECK(trail.code == 0);
  CHECK(trail.err.empty());
  CHECK(trail.out == "audit trail for task:99  (0 entries)\n  (no audit_log entries)\n");

  auto const trail_json = dispatch(fx, {"audit", "trail", "99", "--json"});
  CHECK(trail_json.code == 0);
  // No `commits` and no `agent_activity` keys — an entity with no sessions
  // and no agent rows omits both sub-objects rather than emitting empty ones.
  CHECK(trail_json.out == "{\"entity_kind\":\"task\",\"entity_id\":99,\"entries\":[]}\n");

  auto const session = dispatch(fx, {"audit", "session", "99"});
  CHECK(session.code == 1);
  CHECK(session.err == "error: session 99 not found\n");
}

TEST_CASE("audit trail's four argv refusals each carry their own code and wording", "[cmd][audit][trail][refusal]") {
  auto const fx = make_fixture("refusals");
  seed_trail(fx);

  // Neither form selected.
  auto const neither = dispatch(fx, {"audit", "trail"});
  CHECK(neither.code == 2);
  CHECK(neither.err == "error: audit trail requires <entity-id> or --link <id>\n");
  CHECK(neither.out.empty());

  // A non-integer positional. This is the argv `statediff.t.cpp` feeds as
  // `audit trail task:1`, so the wording is load-bearing there too.
  auto const bad_entity = dispatch(fx, {"audit", "trail", "task:1"});
  CHECK(bad_entity.code == 2);
  CHECK(bad_entity.err == "error: entity id must be an integer, got 'task:1'\n");

  // A non-integer `--link`, which does NOT reuse the message above.
  auto const bad_link = dispatch(fx, {"audit", "trail", "--link", "abc"});
  CHECK(bad_link.code == 2);
  CHECK(bad_link.err == "error: invalid --link value 'abc'\n");

  // The empty string is a non-integer too, not "no link given".
  auto const empty_link = dispatch(fx, {"audit", "trail", "--link", ""});
  CHECK(empty_link.code == 2);
  CHECK(empty_link.err == "error: invalid --link value ''\n");

  // A WELL-FORMED link id naming no row: exit 1, not 2. The pair with
  // `bad_link` above is the point — two bad `--link` values, two codes.
  auto const missing_link = dispatch(fx, {"audit", "trail", "--link", "99"});
  CHECK(missing_link.code == 1);
  CHECK(missing_link.err == "error: external link 99 not found\n");
  // `--json` does not wrap the refusal in an envelope.
  auto const missing_link_json = dispatch(fx, {"audit", "trail", "--link", "99", "--json"});
  CHECK(missing_link_json.code == 1);
  CHECK(missing_link_json.err == "error: external link 99 not found\n");
  CHECK(missing_link_json.out.empty());
}

TEST_CASE("audit trail does NOT validate --kind, unlike search", "[cmd][audit][trail][kind]") {
  auto const fx = make_fixture("kind");
  seed_trail(fx);

  // `search --kind bogus` refuses at exit 2 naming the value. This one
  // succeeds with zero rows. Same flag spelling, same shape of typo, two
  // families, two answers.
  auto const bogus = dispatch(fx, {"audit", "trail", "1", "--kind", "bogus"});
  CHECK(bogus.code == 0);
  CHECK(bogus.err.empty());
  CHECK(bogus.out == "audit trail for bogus:1  (0 entries)\n  (no audit_log entries)\n");

  // The empty string is passed through as the kind rather than defaulting
  // to `task`, so the header renders with nothing before the colon.
  auto const empty = dispatch(fx, {"audit", "trail", "1", "--kind", ""});
  CHECK(empty.code == 0);
  CHECK(empty.out == "audit trail for :1  (0 entries)\n  (no audit_log entries)\n");
}
