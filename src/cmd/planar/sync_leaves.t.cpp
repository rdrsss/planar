// @file sync_leaves.t.cpp
// @brief End-to-end tests for the `sync pull`, `sync push` and
// `sync resolve` leaves wired by plan 996, task 6294.
//
// ## THE FIXTURE'S OWN SHAPE IS ASSERTED BEFORE ANY BYTE COMPARISON RUNS
//
// `seed()` ends by reading its rows back and CHECKing them, and the first
// TEST_CASE below asserts nothing but the fixture. That is not ceremony.
// The `external_links` row here is INSERTED WITH SQL, because the only two
// verbs that create one — `link` and `ext create` — are both unported, so
// there is no in-process CLI path to it. A hand-written insert is exactly
// the shape that fails silently: a row rejected by a CHECK constraint
// leaves the table empty, every case below then matches nothing, and
// `sync pull --all` prints "no links matched" and exits 0 in all of them.
// Every assertion would pass and the leaves would be untested.
//
// ## THE ADAPTER FAILURE ARM AND THE ENGINE FAILURE ARM ARE DIFFERENT ARMS
//
// This is the finding that most shaped the port, and it is the reason the
// exit codes below are not what they look like they should be:
//
//   * a TRANSPORT failure (nothing listening) is NOT an error return. The
//     engine folds it into the RESULT as `outcome=error`, `detail` =
//     `TransportFailed`, writes a `sync_events` row, and returns success.
//     The verb prints an error LINE and exits **0**.
//   * a READ-ONLY push IS an error return, from a pre-flight check before
//     anything is sent. It sets the run's failure flag and the verb exits
//     **1** with "one or more push errors" on stderr.
//
// So `sync pull` against a dead port exits 0 while `sync push` on a
// read-only link exits 1, and BOTH print a line whose text begins
// `  link N: error — `. Reading either case alone would produce a
// confidently wrong model of the verb. Both are asserted, adjacently.
//
// ## THE HTTP CASES USE THE IN-PROCESS FIXTURE SERVER, NOT A STUB
//
// `http/fixture_server.hpp` binds 127.0.0.1 on an ephemeral port, so these
// cases exercise `curl_transport` for real without reaching the network —
// there is no hostname to resolve and no route off the machine. A transport
// stub would bypass every line of the HTTP client. No credential here is
// real: the token is the literal `tok-abc` and the fixture never reads it.
//
// ## PROVENANCE
//
// Every expected byte below was captured from `zig/zig-out/bin/planar` in a
// 38-case differential (28 offline + 10 over the fixture server) that
// matched on stdout, stderr AND exit code in all 38. None was reconstructed
// from what the C++ implementation happens to produce.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

#include "../lib/http/fixture_server.hpp"

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
                         std::format("planar_sync_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
/// @param extra Extra environment variables for this invocation only.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args, std::map<std::string, std::string, std::less<>> extra = {})
    -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  auto vars = fx.vars;
  for (auto const& [key, value] : extra) {
    vars[key] = value;
  }

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run one statement against the fixture database, failing loudly.
/// @param conn The connection.
/// @param sql The statement.
void exec(planar::db::connection& conn, std::string_view sql) {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// @brief Count rows in one table.
/// @param conn The connection.
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

/// @brief Seed a system, a task, a plan and TWO links: one `two-way` and
/// one `read-only`.
///
/// The two directions are both needed and neither is decoration:
/// `all_pullable` accepts both while `all_pushable` accepts only the
/// `two-way` one, so `sync push --all` returning ONE row where
/// `sync pull --all` returns TWO is the only evidence that the two engine
/// queries were not transposed.
/// @param fx The fixture.
/// @param base_url The registered system's base URL.
void seed(const fixture& fx, std::string_view base_url) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", std::string(base_url), "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Demo task", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Demo plan", "--json"}).code == 0);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  // `link` and `ext create` are both unported, so there is no CLI path to an
  // `external_links` row. See this file's header for why the read-back below
  // is load-bearing rather than tidy.
  exec(*conn, "insert into external_links (id, entity_kind, entity_id, system_id, external_id, link_role, "
              "sync_direction, created_at) values "
              "(1, 'task', 1, 1, 'DEMO-1', 'mirror', 'two-way', '2026-07-01T00:00:00.000Z'), "
              "(2, 'plan', 1, 1, 'DEMO-2', 'mirror', 'read-only', '2026-07-01T00:00:00.000Z')");
  REQUIRE(count(*conn, "external_links") == 2);
  REQUIRE(count(*conn, "external_systems") == 1);
  REQUIRE(count(*conn, "tasks") == 1);
  REQUIRE(count(*conn, "plans") == 1);
}

/// @brief A base URL pointing at a port nothing listens on.
///
/// Port 9 is `discard`, which is not bound on a stock macOS or Linux host,
/// so a connection is REFUSED immediately rather than timing out. That makes
/// the transport-failure arm both deterministic and fast, and it reaches no
/// network: 127.0.0.1 is the loopback interface.
constexpr std::string_view k_dead_url = "http://127.0.0.1:9";

/// @brief The canned Jira issue the fixture server returns.
/// @param summary The `fields.summary` value.
/// @param updated The `fields.updated` value, which is the version marker.
/// @return The response body.
auto issue_body(std::string_view summary, std::string_view updated) -> std::string {
  return std::format(R"({{"key":"DEMO-1","fields":{{"summary":"{}","status":{{"name":"To Do"}},"updated":"{}"}}}})", summary,
                     updated);
}

} // namespace

TEST_CASE("the sync fixture seeds the two links its cases read", "[cmd][sync][fixture]") {
  // Deliberately first and deliberately about nothing else. Every other case
  // in this file is a byte comparison that a silently-empty fixture would
  // satisfy by matching nothing; this one fails instead.
  auto const fx = make_fixture("fixture");
  seed(fx, k_dead_url);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(count(*conn, "external_links") == 2);

  // And the rows are the ones the cases assume: two-way first, read-only
  // second. A transposed pair would leave both counts right and every
  // direction-dependent case wrong.
  auto stmt = conn->prepare("select id, sync_direction from external_links order by id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  CHECK(stmt->column_int64(0) == 1);
  CHECK(stmt->column_text(1) == "two-way");
  REQUIRE(stmt->step().value() == planar::db::step_result::row);
  CHECK(stmt->column_int64(0) == 2);
  CHECK(stmt->column_text(1) == "read-only");
}

TEST_CASE("pull and push refuse an absent target, naming their own verb", "[cmd][sync][refusal]") {
  auto const fx = make_fixture("noargs");
  seed(fx, k_dead_url);

  auto const pull = dispatch(fx, {"sync", "pull"});
  CHECK(pull.code == 2);
  CHECK(pull.out.empty());
  CHECK(pull.err == "error: sync pull requires <link-id>, <kind:id>, or --all\n");

  // The verb NAMES ITSELF. A shared message would read "sync pull" on both.
  auto const push = dispatch(fx, {"sync", "push"});
  CHECK(push.code == 2);
  CHECK(push.err == "error: sync push requires <link-id>, <kind:id>, or --all\n");
}

TEST_CASE("a malformed target is exit 2 while a well-formed missing one is exit 1", "[cmd][sync][refusal][exitcodes]") {
  auto const fx = make_fixture("badref");
  seed(fx, k_dead_url);

  // Four DIFFERENT malformations that all land on one message: no colon, an
  // empty kind, an unknown kind, and a kind that exists in the wider
  // entity-link vocabulary but NOT in the narrower external-link one.
  // `plan_step` is the interesting one — it is a real Planar entity kind and
  // is still rejected here, which is the `external_entity_kind` boundary
  // doing its job rather than a typo being caught.
  //
  // The last four are the multi-colon and case boundary. They are pinned
  // because `parse_kind_id_ref` is documented as splitting on the LAST
  // colon, and a break-probe that reversed the scan direction SURVIVED the
  // whole suite. It turns out to be an EQUIVALENT mutant rather than a
  // coverage gap — no `external_entity_kind` spelling contains a colon, so
  // a forward scan and a backward scan reject exactly the same inputs — and
  // all four were confirmed against the oracle before being written down.
  // They stay so the boundary is stated rather than assumed.
  for (auto const* bad : {"not-a-ref", ":5", "nosuch:5", "plan_step:5", "task:1:2", ":task:5", "task:5:", "TASK:5"}) {
    INFO("malformed target: " << bad);
    auto const got = dispatch(fx, {"sync", "pull", bad});
    CHECK(got.code == 2);
    CHECK(got.err == "error: invalid sync target; expected <link-id> or <kind:id>\n");
  }

  // A well-formed link id that does not exist is a different bucket: exit 1,
  // and a different message.
  auto const missing = dispatch(fx, {"sync", "pull", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: sync target not found\n");

  // But a well-formed ENTITY ref with no links is not an error at all — it
  // is an empty match, exit 0. Three shapes, three outcomes.
  auto const no_links = dispatch(fx, {"sync", "pull", "task:99"});
  CHECK(no_links.code == 0);
  CHECK(no_links.out == "no links matched\n");
  CHECK(no_links.err.empty());
}

TEST_CASE("an empty match set prints a line in text mode and NOTHING under --json", "[cmd][sync][json][empty]") {
  auto const fx = make_fixture("emptyset");
  seed(fx, k_dead_url);

  auto const text = dispatch(fx, {"sync", "pull", "task:99"});
  CHECK(text.code == 0);
  CHECK(text.out == "no links matched\n");

  // NOT `[]`, not `{"links":[]}`, not the text line — zero bytes. A consumer
  // parsing stdout gets nothing, which is the oracle's contract and is the
  // sort of thing a port "tidies up" by accident.
  auto const json = dispatch(fx, {"sync", "pull", "task:99", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.empty());
  CHECK(json.err.empty());
}

TEST_CASE("a transport failure is a RESULT at exit 0, not an error at exit 1", "[cmd][sync][pull][transport]") {
  auto const fx = make_fixture("deadport");
  seed(fx, k_dead_url);

  // Nothing is listening on 127.0.0.1:9, so the adapter cannot reach the
  // provider — and the verb still exits 0. See this file's header.
  auto const text = dispatch(fx, {"sync", "pull", "1"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(text.code == 0);
  CHECK(text.err.empty());
  // TWO leading spaces, and the separator is an EM DASH.
  CHECK(text.out == "  link 1: error — TransportFailed\n");

  auto const json = dispatch(fx, {"sync", "pull", "1", "--json"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(json.code == 0);
  CHECK(json.out == "{\"link_id\":1,\"outcome\":\"error\",\"fields_changed\":[],\"detail\":\"TransportFailed\"}\n");
}

TEST_CASE("a read-only push IS an error arm, at exit 1, beside the exit-0 one", "[cmd][sync][push][readonly]") {
  auto const fx = make_fixture("readonly");
  seed(fx, k_dead_url);

  // Link 2 is `read-only`. The refusal happens BEFORE any request, so the
  // dead port is irrelevant here — and this is the arm that sets the run's
  // failure flag, which the transport failure above does not.
  auto const text = dispatch(fx, {"sync", "push", "2"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(text.code == 1);
  CHECK(text.out == "  link 2: error — ReadOnly\n");
  CHECK(text.err == "error: one or more push errors\n");

  // The per-link line goes to STDOUT even on the failing run, and `--json`
  // still produces a well-formed object for it. Only the summary is stderr.
  auto const json = dispatch(fx, {"sync", "push", "2", "--json"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(json.code == 1);
  CHECK(json.out == "{\"link_id\":2,\"outcome\":\"error\",\"fields_changed\":[],\"detail\":\"ReadOnly\"}\n");
  CHECK(json.err == "error: one or more push errors\n");
}

// A Catch2 title may not START with `--`: ctest passes the name straight to
// Catch2's own CLI, which parses the leading dashes as an option and fails
// the test with "Unrecognised token" before a single assertion runs. Both
// flag-named cases below therefore lead with a word. (Discovered here the
// hard way — the case was red under ctest and green when run by tag.)
TEST_CASE("with --all, pull selects two links and push only the two-way one", "[cmd][sync][all][direction]") {
  auto const fx = make_fixture("alldir");
  seed(fx, k_dead_url);

  // The asymmetry IS the assertion: `all_pullable` takes read-only and
  // two-way, `all_pushable` takes two-way only. Transposing the two engine
  // queries would leave both verbs working and both counts wrong.
  auto const pulled = dispatch(fx, {"sync", "pull", "--all"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(pulled.code == 0);
  CHECK(pulled.out == "  link 1: error — TransportFailed\n"
                      "  link 2: error — TransportFailed\n");

  auto const pushed = dispatch(fx, {"sync", "push", "--all"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(pushed.code == 0);
  CHECK(pushed.out == "  link 1: error — TransportFailed\n");
}

TEST_CASE("the --system flag filters by slug and refuses an unknown one at exit 1", "[cmd][sync][system]") {
  auto const fx = make_fixture("system");
  seed(fx, k_dead_url);

  auto const matched = dispatch(fx, {"sync", "pull", "--all", "--system", "jira-demo"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(matched.code == 0);
  CHECK(matched.out == "  link 1: error — TransportFailed\n"
                       "  link 2: error — TransportFailed\n");

  // An unknown slug is NOT an empty filter result — it is a refusal, and it
  // carries the Zig error NAME into operator-visible text.
  auto const unknown = dispatch(fx, {"sync", "pull", "--all", "--system", "nope"});
  CHECK(unknown.code == 1);
  CHECK(unknown.err == "error: sync pull: system 'nope': NotFound\n");
}

TEST_CASE("resolve refuses a bad id, a bad --keep and a missing event, in three buckets", "[cmd][sync][resolve][refusal]") {
  auto const fx = make_fixture("resolvebad");
  seed(fx, k_dead_url);

  auto const bad_id =
      dispatch(fx, {"sync", "resolve", "abc", "--keep", "local", "--evidence-token", "t", "--expected-local-updated-at", "v"});
  CHECK(bad_id.code == 2);
  CHECK(bad_id.err == "error: invalid event-id 'abc'\n");

  auto const bad_keep =
      dispatch(fx, {"sync", "resolve", "1", "--keep", "sideways", "--evidence-token", "t", "--expected-local-updated-at", "v"});
  CHECK(bad_keep.code == 2);
  CHECK(bad_keep.err == "error: --keep must be 'local' or 'remote', got 'sideways'\n");

  // Exit 1, not 2: a well-formed id for an event that does not exist is a
  // lookup failure, not operator error.
  auto const missing =
      dispatch(fx, {"sync", "resolve", "4242", "--keep", "local", "--evidence-token", "t", "--expected-local-updated-at", "v"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: sync event 4242 not found\n");
}

TEST_CASE("the evidence CAS guard fires ahead of the is-this-a-conflict check", "[cmd][sync][resolve][guards]") {
  // ## THIS CASE ASSERTED SOMETHING ELSE FIRST, AND THE ORACLE SAID NO
  //
  // It originally claimed that naming a NON-conflict event refuses with
  // "sync event N is not a conflict; nothing to resolve" at exit 2 — which
  // is a message and an exit code the verb genuinely has, and which reads as
  // the obvious behaviour. It is not reachable. A three-case differential
  // against `zig/zig-out/bin/planar`, over the exact two `sync_events` rows
  // seeded below, showed the ORACLE refusing the `ok` event with the
  // EVIDENCE message at exit 3, byte for byte identical to this binary.
  //
  // So the evidence compare-and-swap is checked before the event's outcome
  // is looked at, and an operator who names the wrong event is told their
  // token is stale rather than that they picked a non-conflict. That is
  // worth an oracle-side task row (it is a genuinely misleading diagnostic),
  // but it is the oracle's behaviour and is reproduced deliberately.
  //
  // The engine module's own header documents the guard order as
  // stale-then-token; that ordering does not survive contact with this
  // fixture either. The captured behaviour is what is pinned here.
  auto const fx = make_fixture("resolveguards");
  seed(fx, k_dead_url);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  // Event 1 is an `ok` pull; event 2 is a `conflict` and is the LATEST for
  // the link.
  exec(*conn, "insert into sync_events (id, link_id, direction, outcome, fields_changed, detail, context_json, at) values "
              "(1, 1, 'pull', 'ok', null, null, null, '2026-06-01T00:00:00.000Z'), "
              "(2, 1, 'pull', 'conflict', '[\"title\"]', null, null, '2026-06-02T00:00:00.000Z')");
  REQUIRE(count(*conn, "sync_events") == 2);

  constexpr std::string_view k_evidence_refusal = "; inspect and approve fresh evidence\n";

  // The non-conflict event.
  auto const on_ok =
      dispatch(fx, {"sync", "resolve", "1", "--keep", "local", "--evidence-token", "t", "--expected-local-updated-at", "v"},
               {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(on_ok.code == 3);
  CHECK(on_ok.out.empty());
  CHECK(on_ok.err == std::format("error: sync event 1 evidence or approved local version changed{}", k_evidence_refusal));

  // The genuine conflict event with a token that cannot match. Same bucket,
  // same message shape, different id — which is the point: these two inputs
  // are NOT distinguishable from the operator's side.
  auto const on_conflict = dispatch(
      fx, {"sync", "resolve", "2", "--keep", "local", "--evidence-token", "wrong-token", "--expected-local-updated-at", "v"},
      {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(on_conflict.code == 3);
  CHECK(on_conflict.out.empty());
  CHECK(on_conflict.err == std::format("error: sync event 2 evidence or approved local version changed{}", k_evidence_refusal));

  // Nothing was mutated by either refusal — a CAS that refused but still
  // wrote would pass both assertions above.
  CHECK(count(*conn, "sync_events") == 2);
}

TEST_CASE("an adapter-build failure is the RAW Zig tag at exit 1 on all three verbs", "[cmd][sync][factory]") {
  // ## THIS IS THE BUG THIS CYCLE ACTUALLY FOUND, AND IT NEEDS ITS OWN CASE
  //
  // The port originally rendered a factory refusal through
  // `factory_error_message`, because that is what `ext test` — the only
  // other wired `ext`/`sync` leaf, and so the obvious model — does. It
  // produces prose ("token env var 'DEMO_TOKEN' is not set") at exit 2.
  // These three verbs instead interpolate the RAW `@errorName` tag and land
  // at exit 1. Deriving from the sibling rather than the oracle was wrong.
  //
  // The parity lane caught it, then STOPPED covering it: that lane's
  // harness pins a fixed environment with no extension point, so the fixture
  // had to register an always-set auth variable to reach the result-stream
  // cases at all — which means it never takes this path again. A
  // break-probe replacing `factory_error_name(...)` with a literal SURVIVED
  // against the whole suite. This case is what kills it.
  //
  // All four expectations captured from `zig/zig-out/bin/planar` with no
  // token in the environment; all four matched on stdout, stderr and exit.
  auto const fx = make_fixture("factory");
  seed(fx, k_dead_url);

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  exec(*conn, "insert into sync_events (id, link_id, direction, outcome, fields_changed, detail, context_json, at) values "
              "(1, 1, 'pull', 'conflict', '[\"title\"]', null, null, '2026-06-02T00:00:00.000Z')");
  REQUIRE(count(*conn, "sync_events") == 1);

  // Note what is NOT passed: `dispatch` adds no `DEMO_TOKEN`, so the
  // registered `token-env` credential cannot resolve.
  auto const pulled = dispatch(fx, {"sync", "pull", "1"});
  CHECK(pulled.code == 1);
  CHECK(pulled.out.empty());
  CHECK(pulled.err == "error: sync pull: build adapter for jira-demo: TokenEnvVarMissing\n");

  // Each verb NAMES ITSELF, so a shared message would read "sync pull" here.
  auto const pushed = dispatch(fx, {"sync", "push", "1"});
  CHECK(pushed.code == 1);
  CHECK(pushed.err == "error: sync push: build adapter for jira-demo: TokenEnvVarMissing\n");

  // And `resolve`'s message omits the system slug that the other two carry
  // — three verbs, three strings, not one template.
  auto const resolved =
      dispatch(fx, {"sync", "resolve", "1", "--keep", "local", "--evidence-token", "t", "--expected-local-updated-at", "v"});
  CHECK(resolved.code == 1);
  CHECK(resolved.err == "error: sync resolve: build adapter: TokenEnvVarMissing\n");

  // The contrast that makes the exit code load-bearing: the SAME refusal on
  // `ext test` is exit 2 with prose. If a future refactor unified the two,
  // one of these two assertions would fail.
  auto const via_ext_test = dispatch(fx, {"ext", "test", "jira-demo"});
  CHECK(via_ext_test.code == 2);
  CHECK(via_ext_test.err == "error: token env var 'DEMO_TOKEN' is not set\n");
}

TEST_CASE("a real HTTP round trip pulls, applies and then conflicts", "[cmd][sync][pull][http]") {
  // The remote's summary and version live here so the "remote changed" step
  // is a variable assignment rather than a second server.
  std::mutex  guard;
  std::string summary = "Demo task";
  std::string updated = "2026-01-01T00:00:00.000+0000";
  std::size_t gets    = 0;

  planar::http::fixture::server remote([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const lock{guard};
    if (req.verb == "PUT") {
      return planar::http::fixture::canned_response{.status = 204, .body = {}, .content_type = "application/json"};
    }
    ++gets;
    return planar::http::fixture::canned_response{
        .status = 200, .body = issue_body(summary, updated), .content_type = "application/json"};
  });

  auto const fx = make_fixture("http");
  seed(fx, remote.base_url());

  // 1. Remote agrees with local, so nothing moves. `noop` — NOT `ok` with an
  //    empty field list, which is what a port that always reported success
  //    would emit.
  auto const first = dispatch(fx, {"sync", "pull", "1"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(first.code == 0);
  CHECK(first.out == "  link 1: noop\n");
  CHECK(first.err.empty());
  // The round trip actually happened. Without this the case above would pass
  // identically against a server that was never contacted.
  {
    std::scoped_lock const lock{guard};
    CHECK(gets == 1);
  }

  // The SAME state under `--json`, because this is the only result in the
  // file whose `detail` is EMPTY — and an empty `detail` is OMITTED from the
  // object rather than rendered as `""` or `null`. Every other JSON
  // assertion here carries a detail, so a renderer that always emitted the
  // key survived a break-probe until this line existed.
  auto const first_json = dispatch(fx, {"sync", "pull", "1", "--json"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(first_json.code == 0);
  CHECK(first_json.out == "{\"link_id\":1,\"outcome\":\"noop\",\"fields_changed\":[]}\n");

  // 2. The remote alone changed, so the change applies cleanly and the
  //    changed field is NAMED in the line.
  {
    std::scoped_lock const lock{guard};
    summary = "Renamed remotely";
    updated = "2026-02-02T00:00:00.000+0000";
  }
  auto const applied = dispatch(fx, {"sync", "pull", "1"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(applied.code == 0);
  CHECK(applied.out == "  link 1: ok — title\n");

  // 3. Both sides changed since the last successful sync: the CONFLICT arm,
  //    which is the ONLY path in either verb that reaches exit 3. It carries
  //    a field list AND a detail, so this is also the only case that pins
  //    both em-dash separators in one line.
  REQUIRE(dispatch(fx, {"task", "update", "1", "--title", "Renamed locally"}).code == 0);
  {
    std::scoped_lock const lock{guard};
    summary = "Renamed remotely again";
    updated = "2026-03-03T00:00:00.000+0000";
  }
  auto const conflicted = dispatch(fx, {"sync", "pull", "1"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(conflicted.code == 3);
  CHECK(conflicted.out == "  link 1: conflict — title — local and remote changed since the last successful sync\n");
  CHECK(conflicted.err == "error: one or more sync conflicts; use 'sync resolve' to settle\n");

  auto const conflicted_json = dispatch(fx, {"sync", "pull", "1", "--json"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(conflicted_json.code == 3);
  CHECK(conflicted_json.out == "{\"link_id\":1,\"outcome\":\"conflict\",\"fields_changed\":[\"title\"],"
                               "\"detail\":\"local and remote changed since the last successful sync\"}\n");
}

TEST_CASE("push sends the local title over a real transport", "[cmd][sync][push][http]") {
  std::mutex  guard;
  std::string last_put_body;
  std::size_t puts = 0;

  planar::http::fixture::server remote([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const lock{guard};
    if (req.verb == "PUT") {
      ++puts;
      last_put_body = req.body;
      return planar::http::fixture::canned_response{.status = 204, .body = {}, .content_type = "application/json"};
    }
    return planar::http::fixture::canned_response{
        .status = 200, .body = issue_body("Demo task", "2026-01-01T00:00:00.000+0000"), .content_type = "application/json"};
  });

  auto const fx = make_fixture("pushhttp");
  seed(fx, remote.base_url());

  auto const pushed = dispatch(fx, {"sync", "push", "1"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(pushed.code == 0);
  CHECK(pushed.out == "  link 1: ok — title\n");
  CHECK(pushed.err.empty());

  std::scoped_lock const lock{guard};
  CHECK(puts == 1);
  // The body carries the LOCAL title, not the remote one. Asserting only the
  // exit code and the rendered line would pass against a push that sent an
  // empty document.
  CHECK(last_put_body.contains("Demo task"));
  CHECK(last_put_body.contains("summary"));
}
