// @file propagate_one_publish_leaves.t.cpp
// @brief In-process tests for `planar ext propagate-one` and `planar
// workbench publish` (plan 996, task 6335).
//
// ## THE FIRST CASE ASSERTS THE FIXTURE
//
// Every case here turns on a live fixture HTTP server and two registered
// systems whose `base_url` points at it. If the `gh-demo` URL rewrite failed,
// each case would fail at the transport and refuse identically — and a
// uniform failure is indistinguishable from a uniform refusal for any other
// reason, so a link-count assertion would pass for the wrong reason. The
// first case pins the server, both systems, the seeded entities and the
// EMPTY link table before any comparison runs. Same discipline as
// `ext_create_leaf.t.cpp` and the `tree` cycle before it.
//
// ## WHAT THIS CYCLE ACTUALLY MEASURED
//
// Both leaves were carried in the unported inventory under "the create /
// propagate half of `engine_extsync`" — 3665 lines across five files. Read
// by SYMBOL rather than by file name:
//
//   ext propagate-one   2 functions, ~40 lines (`strategyForSystem`,
//                       `loadExistingMirror`). Reaches NOTHING in
//                       `parent_issue.zig` or `projects_v2.zig`.
//   workbench publish   1 function, 36 lines (`recordLink`).
//
// That is the same correction already recorded for `audit commits` (6272),
// the `sync` trio (6294) and `plan descendants` (6298).
//
// ## THREE VERBS, THREE DIFFERENT ANSWERS TO "IT ALREADY EXISTS"
//
// This is the finding most worth not generalising, and the cases below pin
// all three from the SERVER'S side, because none of them is visible in
// stdout, stderr or the exit code:
//
//   ext create          POSTs a SECOND ticket, then refuses on the
//                       duplicate link (defect 6313). Also POSTs before
//                       validating `--role` (defect 6312). Both reproduced
//                       under D2, pinned in ext_create_leaf.t.cpp.
//   ext propagate-one   SKIPS. `load_existing_mirror` runs FIRST — before
//                       the template is loaded and before an adapter
//                       exists — so a repeat sends ZERO requests and
//                       returns `op:"skipped"` carrying the existing id.
//   workbench publish   REFUSES on the existing link, before rendering.
//
// `propagate-one` is therefore the correct shape already present in this
// tree, and is what 6312/6313 should be made to look like when they are
// deliberately fixed.
//
// ## ORACLE PROVENANCE
//
// The idempotency ordering was read out of
// `zig/src/cmd/planar/handlers/ext/propagate_one.zig` directly:
// `loadExistingMirror` at line 70 precedes `buildTaskContext` at 89, the
// adapter build at 288 and `createRemote` at 121, and every argument
// refusal (lines 203-233) precedes all four. The oracle's `--strategy`
// refusal wording and the three-arm accept/refuse split are at lines
// 216-228. `templateKindForEntity` discarding `strategy_kind` for GitHub is
// at line 375 (`_ = strategy_kind;`).
//
// The payload is indent-2 JSON, not compact: `render.zig:52-57`'s `toJson`
// passes `.whitespace = .indent_2`. That is what the provider receives, so
// it is asserted on the captured request body rather than assumed.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

// AFTER the imports, not before — the header names `std::function` and
// `std::thread` without including <functional> or <thread> itself.
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
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_p1pub_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"PLANAR_WORKBENCH_ROOT", (root / "wb").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()},
                  {"DEMO_TOKEN", "tok-abc"}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table.
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

/// @brief Read one integer out of the fixture database.
auto scalar(const fixture& fx, std::string_view sql) -> std::int64_t {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Read one text column, or `<none>` when no row matched.
auto text(const fixture& fx, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  if (*step != planar::db::step_result::row) {
    return "<none>";
  }
  return std::string{stmt->column_text(0)};
}

/// @brief The canned create responses, one per provider.
///
/// Endpoint discrimination is itself pinned: a port that built the wrong URL
/// would land on the other provider's branch and return an id of the wrong
/// SHAPE rather than failing outright.
auto respond(const planar::http::fixture::captured_request& req) -> planar::http::fixture::canned_response {
  if (req.target.contains("rest/api/3/issue")) {
    return {.status = 201, .body = R"({"key":"DEMO-77"})", .content_type = "application/json"};
  }
  return {
      .status = 201, .body = R"({"number":42,"html_url":"https://example.invalid/i/42"})", .content_type = "application/json"};
}

/// @brief Register both providers against `base` and seed a plan and a task.
void seed(const fixture& fx, std::string_view base) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", std::string{base}, "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-demo", "--auth-env", "DEMO_TOKEN", "--project", "acme/widgets"}).code ==
          0);
  // `ext register github` takes no `--base-url`, so the row it writes points
  // at the real api.github.com. Redirecting it is what keeps this suite OFF
  // the network.
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", base)).has_value());
  }
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor plan", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Demo task", "--plan", "1", "--json"}).code == 0);
}

} // namespace

TEST_CASE("the propagate-one fixture points BOTH systems at the local server", "[cmd][ext][propagate-one][fixture]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("fixture");
  seed(fx, remote.base_url());

  CHECK(scalar(fx, "select count(*) from external_systems") == 2);
  CHECK(text(fx, "select base_url from external_systems where slug = 'jira-demo'") == remote.base_url());
  CHECK(text(fx, "select base_url from external_systems where slug = 'gh-demo'") == remote.base_url());
  // The PRESENT case for the entities, so a later "no link was written"
  // assertion cannot pass because the entity was missing all along.
  CHECK(scalar(fx, "select count(*) from plans") == 1);
  CHECK(scalar(fx, "select count(*) from tasks") == 1);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
  // Nothing has been sent yet, which is the baseline every request-count
  // assertion below is measured against.
  CHECK(remote.request_count() == 0);
}

TEST_CASE("propagate-one creates a Jira counterpart and records the mirror link", "[cmd][ext][propagate-one][jira]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("jira");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate-one", "jira-demo", "--from", "task:1", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out.contains(R"("op":"created")"));
  CHECK(ran.out.contains(R"("external_id":"DEMO-77")"));
  CHECK(ran.out.contains(R"("entity_kind":"task")"));
  // Jira resolves to `jira-epic` even though the RENDERED template kind for a
  // task is `sub-task`: the strategy names the family, not the template.
  CHECK(ran.out.contains(R"("strategy":"jira-epic")"));

  CHECK(scalar(fx, "select count(*) from external_links") == 1);
  CHECK(text(fx, "select external_id from external_links where id = 1") == "DEMO-77");
  CHECK(text(fx, "select link_role from external_links where id = 1") == "mirror");
  // `read-only` is this verb's default, NOT `ext create`'s `two-way`. Two
  // creation verbs on one table with different default directions.
  CHECK(text(fx, "select sync_direction from external_links where id = 1") == "read-only");
  // A TASK is not the anchor, so no strategy cache is written.
  CHECK(text(fx, "select coalesce(config_json,'<null>') from external_links where id = 1") == "<null>");
  CHECK(remote.request_count() == 1);
}

TEST_CASE("propagate-one caches the strategy on the ANCHOR plan only", "[cmd][ext][propagate-one][anchor]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("anchor");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate-one", "jira-demo", "--from", "plan:1", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out.contains(R"("op":"created")"));
  // Plan 1 has no parent, so it is the anchor and DOES carry the cache. This
  // is the present case for the assertion the task-case above makes in the
  // negative.
  CHECK(text(fx, "select config_json from external_links where id = 1") == R"({"strategy":"jira-epic"})");
}

TEST_CASE("a repeated propagate-one SKIPS and sends nothing", "[cmd][ext][propagate-one][idempotent]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("idem");
  seed(fx, remote.base_url());

  auto const first = dispatch(fx, {"ext", "propagate-one", "jira-demo", "--from", "task:1", "--json"});
  REQUIRE(first.code == 0);
  REQUIRE(first.out.contains(R"("op":"created")"));
  REQUIRE(remote.request_count() == 1);

  auto const again = dispatch(fx, {"ext", "propagate-one", "jira-demo", "--from", "task:1", "--json"});
  CHECK(again.code == 0);
  CHECK(again.err.empty());
  // `skipped`, carrying the EXISTING id rather than a new one.
  CHECK(again.out.contains(R"("op":"skipped")"));
  CHECK(again.out.contains(R"("external_id":"DEMO-77")"));

  // THE ASSERTION THIS CASE EXISTS FOR, and it is invisible in the output
  // above: the repeat sent NOTHING. `ext create` in the same situation POSTs
  // a second ticket (defect 6313). Still 1, not 2.
  CHECK(remote.request_count() == 1);
  CHECK(scalar(fx, "select count(*) from external_links") == 1);
}

TEST_CASE("propagate-one refuses every bad argument BEFORE contacting the remote",
          "[cmd][ext][propagate-one][refusal][ordering]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("refuse");
  seed(fx, remote.base_url());

  // `parent-issue` and `projects-v2` are recognised ONLY to refuse them with
  // the redirect to `ext propagate --github-strategy`.
  auto const parent = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "task:1", "--strategy", "parent-issue"});
  CHECK(parent.code == 2);
  CHECK(parent.err.contains("is not supported by propagate-one"));
  CHECK(parent.err.contains("ext propagate --github-strategy parent-issue"));

  auto const projects = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "task:1", "--strategy", "projects-v2"});
  CHECK(projects.code == 2);
  CHECK(projects.err.contains("is not supported by propagate-one"));

  // Anything else is the generic invalid-value refusal, naming the ONE
  // accepted value.
  auto const bogus = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "task:1", "--strategy", "nonsense"});
  CHECK(bogus.code == 2);
  CHECK(bogus.err.contains("accepted: tracking-issue"));

  auto const bad_sync = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "task:1", "--sync", "sideways"});
  CHECK(bad_sync.code == 2);
  CHECK(bad_sync.err.contains("read-only, write-back, two-way"));

  auto const bad_ref = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "notacolon"});
  CHECK(bad_ref.code == 2);
  CHECK(bad_ref.err.contains("expected kind:integer-id"));

  auto const bad_kind = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "decision:1"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err.contains("accepted: plan, task"));

  auto const missing = dispatch(fx, {"ext", "propagate-one", "gh-demo", "--from", "task:999"});
  CHECK(missing.code == 1);
  CHECK(missing.err.contains("task:999 not found"));

  // THE POINT OF THE CASE: seven refusals, ZERO requests. `ext create`
  // POSTs before validating `--role` (defect 6312); this verb does not.
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("a propagate-one dry run renders but builds no adapter and sends nothing", "[cmd][ext][propagate-one][dry-run]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("dry");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "propagate-one", "jira-demo", "--from", "task:1", "--dry-run", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out.contains(R"("op":"planned")"));
  // The placeholder is the TEMPLATE KIND in angle brackets — `sub-task` for a
  // task on Jira, which is also what proves the template mapping ran.
  CHECK(ran.out.contains(R"("external_id":"<sub-task>")"));
  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("the propagate-one request carries the bearer token and an indent-2 body", "[cmd][ext][propagate-one][http][headers]") {
  std::vector<planar::http::fixture::captured_request> seen;
  std::mutex                                           guard;
  planar::http::fixture::server                        remote([&](const planar::http::fixture::captured_request& req) {
    {
      std::scoped_lock const lock(guard);
      seen.push_back(req);
    }
    return respond(req);
  });
  auto const                                           fx = make_fixture("headers");
  seed(fx, remote.base_url());

  REQUIRE(dispatch(fx, {"ext", "propagate-one", "jira-demo", "--from", "task:1", "--json"}).code == 0);

  std::scoped_lock const lock(guard);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].verb == "POST");
  CHECK(seen[0].target.contains("/rest/api/3/issue"));
  CHECK(seen[0].header_value("authorization") == "Bearer tok-abc");
  CHECK(seen[0].header_value("content-type") == "application/json");
  // Jira's Accept, not GitHub's — the two differ and the discrimination is
  // part of what `create_remote` is responsible for.
  CHECK(seen[0].header_value("accept") == "application/json");
  // INDENT-2, not compact: the oracle's `toJson` passes `.indent_2` and the
  // provider receives these exact bytes. A compact serializer would still
  // parse and still create the ticket, so nothing else here would catch it.
  CHECK(seen[0].body.contains("\n  "));
}

TEST_CASE("workbench publish records a mirror link AND its sync event", "[cmd][workbench][publish]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("publish");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"workbench", "publish", "1", "--system", "jira-demo", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out.contains(R"("ok":true)"));
  CHECK(ran.out.contains(R"("external_id":"DEMO-77")"));
  CHECK(ran.out.contains(R"("link_id":1)"));

  CHECK(scalar(fx, "select count(*) from external_links") == 1);
  CHECK(text(fx, "select link_role from external_links where id = 1") == "mirror");
  CHECK(text(fx, "select sync_direction from external_links where id = 1") == "two-way");
  CHECK(text(fx, "select last_sync_status from external_links where id = 1") == "ok");

  // THE ASSERTION THAT SEPARATES `record_mirror_link` FROM `link::create`:
  // the audit event. `create` writes external_links and nothing else, so a
  // port that reused it would pass every check above and fail only here.
  CHECK(scalar(fx, "select count(*) from sync_events where link_id = 1 and direction = 'push' and outcome = 'ok'") == 1);
  // `fields_changed` and `detail` stay NULL — the oracle's three-column
  // insert, not `update_sync_direction`'s five-column one.
  CHECK(scalar(fx, "select count(*) from sync_events where link_id = 1 and fields_changed is null and detail is null") == 1);
}

TEST_CASE("workbench publish REFUSES a second publication rather than skipping it", "[cmd][workbench][publish][duplicate]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("dup");
  seed(fx, remote.base_url());

  REQUIRE(dispatch(fx, {"workbench", "publish", "1", "--system", "jira-demo", "--json"}).code == 0);
  REQUIRE(remote.request_count() == 1);

  auto const again = dispatch(fx, {"workbench", "publish", "1", "--system", "jira-demo", "--json"});
  // The THIRD of the three duplicate shapes: not `ext create`'s second POST,
  // not `propagate-one`'s silent skip — a refusal.
  CHECK(again.code != 0);
  CHECK(again.err.contains("already has an external link on jira-demo"));
  // And it refuses BEFORE sending: still 1.
  CHECK(remote.request_count() == 1);
  CHECK(scalar(fx, "select count(*) from external_links") == 1);
}

TEST_CASE("workbench publish refuses an unknown plan and an unknown system", "[cmd][workbench][publish][refusal]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("pubrefuse");
  seed(fx, remote.base_url());

  auto const no_plan = dispatch(fx, {"workbench", "publish", "999", "--system", "jira-demo"});
  CHECK(no_plan.code != 0);
  CHECK(no_plan.err.contains("plan not found: 999"));

  auto const no_system = dispatch(fx, {"workbench", "publish", "1", "--system", "nope"});
  CHECK(no_system.code != 0);
  CHECK(no_system.err.contains("external system 'nope' not found"));

  CHECK(remote.request_count() == 0);
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}
