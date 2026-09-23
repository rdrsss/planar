// @file dashboard_leaf.t.cpp
// @brief In-process tests for `planar dashboard`, ported by plan 996,
// task 6329.
//
// ## PARKED FOR SIX MILESTONES BEHIND A BLOCKER THAT HAD BEEN REMOVED
//
// Task 6102 recorded this leaf as needing "the layer-3 cmd surface", and
// src/cmd/planar/CMakeLists.txt's own header still names `dashboard` in
// its blocked-on-layer-3 list. That was true when written. Layer 3 landed
// at task 6105 and nothing revisited the note, so the leaf sat in
// `unported_paths()` behind a condition that had already been satisfied.
// Every engine symbol it needs — `plan::list_plans`, `agentactivity`'s
// claim and next-work readers, `agentrender`'s two fragment writers — was
// present the whole time. The failure mode is a blocker note that ages out
// silently, and it is worth a header rather than a deletion.
//
// ## `--agents` SELECTS A DIFFERENT PAYLOAD, IT DOES NOT ADD FIELDS
//
// Without it: `{"active_plans":[…]}` and nothing else. Not an empty
// `claims`, not an empty `next_available_by_plan`. Pinned as an exact
// payload, because a port that always emits three keys produces valid JSON
// that every scripted caller of the default shape reads differently.
//
// ## THE ACTIVE ARM HAS NO LEASE PREDICATE, SO ONE CLAIM APPEARS TWICE
//
// The oracle's `listActive` filters on `status = 'active'` ALONE. Its
// stale list is `status='stale'` plus expired-lease `active` rows. An
// `active` row whose lease has passed therefore satisfies both, and the
// same claim is emitted in `claims.active` AND `claims.stale` in one
// document. The fixture seeds exactly that row, and both appearances are
// asserted — a port reaching for `list_claims(active)`, which DOES carry
// the lease predicate, produces a defensible-looking output that drops it
// from the active list.
//
// This mirrors `planar-watch ps`, whose module header records the same
// divergence for the same reason; the two arrived at it independently.
//
// ## THREE ABSENCE SPELLINGS IN ONE TEXT LINE
//
// `branch`, `head_sha_at_claim` and `repo_root` fall back to a literal
// `?`; `dirty_at_claim` falls back to `unknown`. The fixture's second
// claim leaves all four NULL so every fallback is observable at once, and
// its first claim populates all four so the present case is asserted too —
// an all-NULL fixture would pass against a port that emitted `?`
// unconditionally.
//
// ## `next_available_by_plan` IS BUILT IN PLAN ORDER, NOT MAP ORDER
//
// It is a JSON object keyed by plan id, which is the shape task 6274's
// hash-iteration finding was about. There is no hazard here: the oracle
// walks the `active_plans` vector, so the keys come out in plan-id order
// deterministically. Asserted as an exact payload with three keys in
// order, which a map-iterating port would fail.
//
// A plan with no available work emits `"<id>":[]` — PRESENT and empty.
// Only a failed query omits a key, which an empty result is not.
//
// ## EVERY CONSTANT CAME FROM `zig/zig-out/bin/planar`
//
// Captured in a pinned scratch arena, both streams through a pipe with the
// exit code read outside it, then diffed byte-for-byte against this binary
// over identical fixtures — including the claim-bearing one below, whose
// rows are inserted as raw SQL on both sides so the two see the same table.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

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
                         std::format("planar_dash_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
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

/// @brief Seed four plans — one per in-flight status plus one `done` — and
/// three tasks.
///
/// Plan 3 is driven active THEN done, because `draft -> done` is an
/// illegal transition. It is the ABSENT twin for the status filter: a
/// fixture of three in-flight plans alone would pass against a port that
/// ignored status entirely.
///
/// Plan 4 (`paused`) is given no tasks, so it is the "present but empty"
/// key in `next_available_by_plan`.
/// @param fx The fixture.
void seed(const fixture& fx) {
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);
  CHECK(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  // ABSOLUTE, never `.`.
  CHECK(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string()}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Alpha", "--slug", "alpha", "--status", "active", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Beta draft", "--slug", "beta", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Gamma", "--slug", "gamma", "--status", "active", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "update", "3", "--status", "done", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "create", "Delta", "--slug", "delta", "--status", "active", "--json"}).code == 0);
  CHECK(dispatch(fx, {"plan", "update", "4", "--status", "paused", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "T one", "--plan", "1", "--slug", "t-one", "--body", "one", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "T two", "--plan", "1", "--slug", "t-two", "--body", "two", "--json"}).code == 0);
  CHECK(dispatch(fx, {"task", "add", "T three", "--plan", "2", "--slug", "t-three", "--body", "three", "--json"}).code == 0);

  // FIXTURE SELF-CHECK. The first seeding attempts on this task used
  // `plan add` and `--title`, both of which this tree's oracle rejects;
  // they wrote NOTHING and every assertion below would have passed
  // against an empty database.
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "plans") == 4);
  CHECK(count(*conn, "tasks") == 3);
  // And the statuses really are what the cases below assume — asserted
  // rather than trusted to `plan update` having taken.
  CHECK(count(*conn, "plans where status = 'done'") == 1);
  CHECK(count(*conn, "plans where status = 'paused'") == 1);
}

} // namespace

TEST_CASE("dashboard rolls up the three in-flight statuses and excludes done", "[cmd][dashboard]") {
  auto const fx = make_fixture("statuses");
  seed(fx);

  auto const text = dispatch(fx, {"dashboard"});
  CHECK(text.code == 0);
  // Three of the four plans. `Gamma` is `done` and appears nowhere — the
  // count and the list agree, which is what makes its absence meaningful.
  CHECK(text.out == "active plans: 3\n"
                    "  plan:1  [active]  Alpha\n"
                    "  plan:2  [draft]  Beta draft\n"
                    "  plan:4  [paused]  Delta\n");
  CHECK(text.err.empty());
  CHECK(text.out.find("Gamma") == std::string::npos);
}

TEST_CASE("the DEFAULT json payload carries active_plans and nothing else", "[cmd][dashboard]") {
  auto const fx = make_fixture("defaultshape");
  seed(fx);

  auto const res = dispatch(fx, {"dashboard", "--json"});
  CHECK(res.code == 0);
  // Neither key is present. A port that emits them empty is valid JSON
  // with a different shape.
  CHECK(res.out.find("\"claims\"") == std::string::npos);
  CHECK(res.out.find("\"next_available_by_plan\"") == std::string::npos);
  CHECK(res.out.starts_with(R"({"active_plans":[{"id":1,)"));
  CHECK(res.out.ends_with("]}\n"));
  // The plan objects are the canonical `plan show --json` shape, sharing
  // the renderer rather than hand-rolling a second one.
  CHECK(res.out.find(R"("slug":"alpha","summary":null,"status":"active","parent_plan_id":null)") != std::string::npos);
  CHECK(res.out.find("gamma") == std::string::npos);

  // ...and `--agents` is what adds them. The present twin for the two
  // absence assertions above.
  auto const agents = dispatch(fx, {"dashboard", "--agents", "--json"});
  CHECK(agents.code == 0);
  CHECK(agents.out.find(R"("claims":{"active":[],"stale":[]})") != std::string::npos);
  CHECK(agents.out.find("\"next_available_by_plan\"") != std::string::npos);
}

TEST_CASE("next_available_by_plan keys are in PLAN order and an empty plan keeps its key", "[cmd][dashboard]") {
  auto const fx = make_fixture("nextwork");
  seed(fx);

  auto const res = dispatch(fx, {"dashboard", "--agents", "--json"});
  CHECK(res.code == 0);
  // The three keys appear in PLAN order — 1, then 2, then 4. Asserted by
  // relative position rather than as one exact payload because the task
  // objects carry timestamps; the ORDER is the property at issue and a
  // map-iterating port would fail it.
  auto const k1 = res.out.find(R"("next_available_by_plan":{"1":[)");
  auto const k2 = res.out.find(R"(,"2":[)");
  auto const k4 = res.out.find(R"(,"4":[]})");
  CHECK(k1 != std::string::npos);
  CHECK(k2 != std::string::npos);
  // Plan 4 has no tasks and still gets its key, with an EMPTY array —
  // present, not omitted. Only a failed query drops a key.
  CHECK(k4 != std::string::npos);
  CHECK(k1 < k2);
  CHECK(k2 < k4);
  CHECK(res.out.ends_with(R"(,"4":[]}})"
                          "\n"));
  // Plan 2's single available task, as the exact object shape the shared
  // `append_task` writer produces, timestamps excepted.
  CHECK(res.out.find(R"({"id":3,"scope_kind":"association","scope_id":1,"plan_id":2,"parent_task_id":null,)"
                     R"("title":"T three","body":"three","slug":"t-three","status":"todo","priority":100,)"
                     R"("next_action":null,"due_at":null,"created_at":)") != std::string::npos);

  // The text arm's tally, which is the same walk reduced to a count.
  auto const text = dispatch(fx, {"dashboard", "--agents"});
  CHECK(text.code == 0);
  CHECK(text.out == "active plans: 3    active claims: 0    stale claims: 0\n"
                    "  plan:1  [active]  Alpha\n"
                    "  plan:2  [draft]  Beta draft\n"
                    "  plan:4  [paused]  Delta\n"
                    "next available by plan:\n"
                    "  plan:1  available:2\n"
                    "  plan:2  available:1\n"
                    "  plan:4  available:0\n");
  // Both claim sections are SUPPRESSED WHEN EMPTY, headers included, while
  // `next available by plan:` prints unconditionally.
  CHECK(text.out.find("active claims:\n") == std::string::npos);
  CHECK(text.out.find("stale claims:\n") == std::string::npos);
}

TEST_CASE("an expired-lease active claim appears in BOTH the active and stale lists", "[cmd][dashboard]") {
  auto const fx = make_fixture("claims");
  seed(fx);

  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("insert into sessions (id, vendor, task_id, started_at) values "
                          "(1,'claude',1,'2026-02-01T00:00:00.000Z')")
                .has_value());
    // Claim 1: unexpired, and every locality column POPULATED.
    REQUIRE(conn->execute("insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, "
                          "claim_scope, status, vendor, repo_root, branch, head_sha_at_claim, dirty_at_claim, "
                          "claimed_at, last_heartbeat_at, lease_expires_at) values "
                          "(1,'tok-active-1',1,'task',1,'exclusive','active','claude','/repo','main',"
                          "'abcdef0123456789','clean','2026-03-01T00:00:00.000Z','2026-03-01T00:00:00.000Z',"
                          "'2099-01-01T00:00:00.000Z')")
                .has_value());
    // Claim 2: status `active`, lease long past, every locality column
    // NULL. This is the row that must appear TWICE.
    REQUIRE(conn->execute("insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, "
                          "claim_scope, status, vendor, claimed_at, last_heartbeat_at, lease_expires_at) values "
                          "(2,'tok-expired-2',1,'task',2,'exclusive','active','codex',"
                          "'2026-02-01T00:00:00.000Z','2026-02-01T00:00:00.000Z','2026-02-01T00:10:00.000Z')")
                .has_value());
    // GUARD: a CHECK-violating insert is dropped SILENTLY and every
    // assertion below would then pin the claim-free answer. `action_kind`
    // is the constrained column that has already cost this milestone a
    // cycle, so the action row is counted too.
    REQUIRE(conn->execute("insert into agent_actions (id, session_id, claim_id, vendor, action_kind, entity_kind, "
                          "entity_id, summary, started_at) values "
                          "(1,1,1,'claude','coder','task',1,'wiring the leaf','2026-03-01T00:05:00.000Z')")
                .has_value());
    CHECK(count(*conn, "agent_work_claims") == 2);
    CHECK(count(*conn, "agent_actions") == 1);
  }

  auto const text = dispatch(fx, {"dashboard", "--agents"});
  CHECK(text.code == 0);
  // Two active, one stale — and `tok-expired-2` is in both sections. The
  // header counts say 2 and 1 over two distinct rows, which is only
  // consistent if one row is double-counted.
  CHECK(text.out == "active plans: 3    active claims: 2    stale claims: 1\n"
                    "  plan:1  [active]  Alpha\n"
                    "  plan:2  [draft]  Beta draft\n"
                    "  plan:4  [paused]  Delta\n"
                    "active claims:\n"
                    // Present case: all four locality columns rendered.
                    "  task:1  vendor:claude  branch:main  sha:abcdef01  dirty:clean  repo:/repo  token:tok-active-1\n"
                    // Absent case: three `?` and one `unknown`, in one line.
                    "  task:2  vendor:codex  branch:?  sha:?  dirty:unknown  repo:?  token:tok-expired-2\n"
                    "stale claims:\n"
                    "  task:2  vendor:codex  branch:?  sha:?  dirty:unknown  repo:?  token:tok-expired-2\n"
                    "next available by plan:\n"
                    // Both of plan 1's tasks are now claimed, so neither is available.
                    "  plan:1  available:0\n"
                    "  plan:2  available:1\n"
                    "  plan:4  available:0\n");

  auto const json = dispatch(fx, {"dashboard", "--agents", "--json"});
  CHECK(json.code == 0);
  // The sha is truncated to 8 in TEXT and emitted WHOLE in JSON — the two
  // renderers disagree, deliberately.
  CHECK(json.out.find(R"("head_sha_at_claim":"abcdef0123456789")") != std::string::npos);
  // `entity_scope` and `latest_action` are BOTH always present on this
  // surface, unlike `planar-agent`'s lean claim object.
  CHECK(json.out.find(R"("entity_scope":{"kind":"association","slug":"project:proj"})") != std::string::npos);
  CHECK(json.out.find(R"("latest_action":{"kind":"coder","summary":"wiring the leaf",)"
                      R"("started_at":"2026-03-01T00:05:00.000Z"})") != std::string::npos);
  // Field PRESENT and null is a third state, distinct from field absent.
  // Claim 2 has no actions and still carries the key.
  CHECK(json.out.find(R"("stage":null,"latest_action":null})") != std::string::npos);
  // The double appearance, in JSON: `tok-expired-2` twice, `tok-active-1`
  // once.
  auto count_of = [&](std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t p = json.out.find(needle); p != std::string::npos; p = json.out.find(needle, p + 1)) {
      ++n;
    }
    return n;
  };
  CHECK(count_of(R"("claim_token":"tok-expired-2")") == 2);
  CHECK(count_of(R"("claim_token":"tok-active-1")") == 1);
}

// NOTE: the title must not begin with `--`. Catch2 forwards the test name
// as an argv token, so a leading `--` makes ctest fail the case with
// "Unrecognised token" while running it by TAG still passes — a green tag
// run and a red suite over the same assertions.
TEST_CASE("the scope flag takes the ASSOCIATION slug and refuses the repo slug", "[cmd][dashboard]") {
  auto const fx = make_fixture("scope");
  seed(fx);

  // PRESENT case: the association slug resolves.
  auto const ok = dispatch(fx, {"dashboard", "--scope", "project:proj", "--json"});
  CHECK(ok.code == 0);
  CHECK(ok.out.find(R"("slug":"alpha")") != std::string::npos);

  // ABSENT case: `proj` is what `scope show` prints as the derived scope
  // (`repo:proj`), and it is NOT an accepted `--scope` spelling here. Exit
  // 1, with the engine's zig error name in the body. Not a defect — the
  // two slug namespaces are different — but it is the shape an operator
  // trips over, so both arms are pinned.
  auto const bad = dispatch(fx, {"dashboard", "--scope", "proj", "--json"});
  CHECK(bad.code == 1);
  CHECK(bad.out == planar::cmd::testsupport::json_error_envelope_line("dashboard", "generic_failure"));
  CHECK(bad.err == "error: plan list: SlugNotFound\n");

  // And `--scope` is NOT comma-split, unlike `plan list --scope`: the
  // whole value is one slug, so a comma-joined pair resolves to nothing.
  auto const csv = dispatch(fx, {"dashboard", "--scope", "project:proj,global", "--json"});
  CHECK(csv.code == 1);
  CHECK(csv.err == "error: plan list: SlugNotFound\n");
}

TEST_CASE("dashboard on an empty database", "[cmd][dashboard]") {
  auto const fx = make_fixture("empty");
  CHECK(dispatch(fx, {"init", "--name", "oracle", "--json"}).code == 0);

  CHECK(dispatch(fx, {"dashboard"}).out == "active plans: 0\n");
  CHECK(dispatch(fx, {"dashboard", "--json"}).out == "{\"active_plans\":[]}\n");
  // The `next available by plan:` header prints with nothing under it.
  CHECK(dispatch(fx, {"dashboard", "--agents"}).out == "active plans: 0    active claims: 0    stale claims: 0\n"
                                                       "next available by plan:\n");
  CHECK(dispatch(fx, {"dashboard", "--agents", "--json"}).out ==
        "{\"active_plans\":[],\"claims\":{\"active\":[],\"stale\":[]},\"next_available_by_plan\":{}}\n");
}
