// @file ext_create_leaf.t.cpp
// @brief In-process tests for the `planar ext create` leaf (plan 996,
// task 6295).
//
// ## THE FIRST CASE ASSERTS THE FIXTURE
//
// The cases below turn on a live fixture HTTP server and a registered
// system whose `base_url` points at it. If the URL rewrite failed, every
// create would fail at the transport and refuse identically — a uniform
// failure that a count-the-links check cannot tell from a uniform refusal
// for any other reason. The first case pins the server, both registered
// systems, and the local entities before any comparison runs.
//
// ## THE CORRECTION THIS LEAF EXISTS TO RECORD
//
// Task 6294 established that the `sync` trio renders adapter-build failures
// as a RAW ZIG TAG at exit 1, and that finding was written down as a rule.
// The rule does NOT transfer. `ext/create.zig:40-45` maps every factory
// error to `error.InvalidInput` with an interpolated message, so `ext
// create` renders like `ext test` — PROSE at exit 2. Captured:
//
//   $Z ext create jira-demo --from task:1   (with DEMO_TOKEN unset)
//       -> exit 2, `error: token env var 'DEMO_TOKEN' is not set`
//
// `factory_error_message` is the right reference here; `factory_error_name`
// in `handlers/sync.cpp` is the wrong one. One family's behaviour was
// generalised into a rule and the rule was wrong two leaves later.
//
// ## ORACLE PROVENANCE
//
// Captured from `zig/zig-out/bin/planar` built at this cycle's base, in a
// pinned scratch arena, against a local fixture server on 127.0.0.1. Exit
// codes were read from the command itself via command substitution with
// `2>file`, never through a pipe. Replayed as a 16-case differential against
// the built C++ binary in a second identically-seeded arena; all 16 agreed
// on stdout, stderr and exit code — AND on the server's own request log, so
// the URL, the three headers and the rendered body are pinned by the
// comparison too, not just the verb's output.
//
// The captures that decided a shape:
//
//   POST /rest/api/3/issue  Authorization: Bearer tok-abc
//       -> `{"key":"DEMO-77"}` becomes external_id `DEMO-77` and
//          external_url `<base>/browse/DEMO-77`.
//
//   POST /repos/acme/widgets/issues  Accept: application/vnd.github+json
//       -> `{"number":42,"html_url":...}` becomes external_id
//          `acme/widgets#42` — the PROJECT and the number, not the number
//          alone — and external_url the `html_url` verbatim.
//
//   $Z ext create jira-demo --from foo:1
//       -> `ext create: read local foo:1: InvalidInput` at exit 2. The
//          `--from` ref is parsed LOOSELY; `handlers/sync.cppm`'s
//          `parse_kind_id_ref` would have refused earlier with a different
//          message, which is why it is deliberately not reused here.
//
//   $Z ext create jira-demo --from task:999
//       -> `ext create: read local task:999: NotFound` at exit ONE. Same
//          message template as the case above, different exit code.
//
// ## THE ORDERING DEFECT IS PINNED FROM THE SERVER'S SIDE
//
// `--role bogus` POSTS THE TICKET and then refuses, leaving a real remote
// issue with no local link; a duplicate `ext create` POSTs a SECOND ticket
// before discovering the existing link. Both were found by reading the
// fixture server's request log rather than the verb's output — nothing in
// stdout, stderr or the exit code reveals either. Both are oracle defects,
// reproduced under D2, and the case at the bottom of this file asserts the
// REQUEST COUNT so that fixing them later is deliberate.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.dispatch;
import planar.cmd.planar_ext.tree;

// AFTER the imports, not before: the header names `std::function` and
// `std::thread` without including <functional> or <thread> itself, so it
// only compiles once `import std;` has been seen. `sync_leaves.t.cpp` orders
// it the same way for the same reason.
#include "../lib/http/fixture_server.hpp"

namespace {

using planar::cmd::ext::context;

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
                         std::format("planar_extcreate_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()},
                  {"DEMO_TOKEN", "tok-abc"}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table.
/// @param fx The fixture.
/// @param args The argv tail.
/// @param drop A variable to REMOVE from the environment for this call only.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args, std::string_view drop = {}) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  auto vars = fx.vars;
  if (!drop.empty()) {
    vars.erase(std::string{drop});
  }

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::ext::map_env(vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::ext::root_app();
  auto const         table = planar::cmd::ext::handlers(*tree);
  int const          code  = planar::cmd::ext::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Read one integer out of the fixture database.
/// @param fx The fixture.
/// @param sql A statement whose first column is the value.
/// @return The value.
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

/// @brief Read one text column out of the fixture database.
/// @param fx The fixture.
/// @param sql A statement whose first column is the value.
/// @return The value, or `<none>` when no row matched.
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
/// Jira answers `{"key":...}` on its own endpoint; GitHub answers
/// `{"number":..,"html_url":..}` on its own. Serving BOTH from one handler
/// keeps a single fixture server for cases that exercise either provider,
/// and the endpoint discrimination is itself part of what is pinned: a port
/// that built the wrong URL would land on the other provider's branch and
/// return an id of the wrong SHAPE rather than failing outright.
/// @param req The captured request.
/// @return The canned response.
auto respond(const planar::http::fixture::captured_request& req) -> planar::http::fixture::canned_response {
  if (req.target.contains("rest/api/3/issue")) {
    return {.status = 201, .body = R"({"key":"DEMO-77"})", .content_type = "application/json"};
  }
  return {
      .status = 201, .body = R"({"number":42,"html_url":"https://example.invalid/i/42"})", .content_type = "application/json"};
}

/// @brief Register both providers against `base` and seed local entities.
/// @param fx The fixture.
/// @param base The fixture server's base URL.
void seed(const fixture& fx, std::string_view base) {
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", std::string{base}, "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-demo", "--auth-env", "DEMO_TOKEN", "--project", "acme/widgets"}).code ==
          0);

  // `ext register github` takes no `--base-url`, so the row it writes points
  // at the real api.github.com. Redirecting it here is what keeps this suite
  // OFF the network; without it the GitHub case would attempt a real call.
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute(std::format("update external_systems set base_url = '{}' where slug = 'gh-demo'", base)).has_value());
  }

  REQUIRE(dispatch(fx, {"plan", "create", "Anchor plan", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Demo task", "--plan", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Demo decision", "--plan", "1", "--body", "because", "--json"}).code == 0);
}

} // namespace

TEST_CASE("the ext create fixture points BOTH systems at the local server", "[cmd][ext][create][fixture]") {
  // Every case below depends on the `gh-demo` URL rewrite in particular: it
  // is the difference between an offline suite and one that tries to reach
  // api.github.com. A silent failure there would surface as a transport
  // refusal that looks like any other refusal.
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("fixture");
  seed(fx, remote.base_url());

  CHECK(scalar(fx, "select count(*) from external_systems") == 2);
  CHECK(text(fx, "select base_url from external_systems where slug = 'jira-demo'") == remote.base_url());
  CHECK(text(fx, "select base_url from external_systems where slug = 'gh-demo'") == remote.base_url());
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
  // The decision row exists, which is what makes the "four kinds, not seven"
  // refusal below a real refusal rather than a missing row.
  CHECK(scalar(fx, "select count(*) from decisions") == 1);
}

TEST_CASE("a Jira create records the key and the derived browse URL", "[cmd][ext][create][jira]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("jira");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:1"});
  CHECK(ran.code == 0);
  CHECK(ran.err.empty());
  CHECK(ran.out == "created DEMO-77 on jira-demo for task:1\n"
                   "link id: 1  (two-way mirror)\n");

  // The URL is DERIVED from the base plus the key — Jira's response carries
  // no URL of its own.
  CHECK(text(fx, "select external_id from external_links where id = 1") == "DEMO-77");
  CHECK(text(fx, "select external_url from external_links where id = 1") == std::format("{}/browse/DEMO-77", remote.base_url()));
  // `ok`, not `never`: this verb just created the counterpart. `link`, which
  // records a ticket created elsewhere, writes `never`.
  CHECK(scalar(fx, "select count(*) from external_links where id = 1 and last_sync_status = 'ok'") == 1);
  // The defaults here are `mirror` / `two-way` — the OPPOSITE of `link`'s
  // `reference` / `read-only`. Two verbs on one table, two default pairs.
  CHECK(scalar(fx, "select count(*) from external_links where id = 1 and link_role = 'mirror'"
                   " and sync_direction = 'two-way'") == 1);
}

TEST_CASE("a GitHub create spells the id as project#number and keeps the html_url", "[cmd][ext][create][github]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("github");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "create", "gh-demo", "--from", "plan:1", "--json"});
  CHECK(ran.code == 0);
  CHECK(ran.out == R"({"ok":true,"link_id":1,"external_id":"acme/widgets#42",)"
                   R"("external_url":"https://example.invalid/i/42","sync_direction":"two-way"})"
                   "\n");

  // The PROJECT is part of the id, not just the number. An id of `42` or
  // `#42` would still be a plausible-looking string and would not round-trip
  // through the adapter's `validate`.
  CHECK(text(fx, "select external_id from external_links where id = 1") == "acme/widgets#42");
  // GitHub's URL is taken VERBATIM from the response, where Jira's is
  // derived. The two providers are not symmetric here.
  CHECK(text(fx, "select external_url from external_links where id = 1") == "https://example.invalid/i/42");
}

TEST_CASE("the request carries the bearer token and the provider's own Accept header", "[cmd][ext][create][http][headers]") {
  // Read from the SERVER's side. None of this is visible in the verb's
  // stdout, so a port that sent no Authorization header at all would pass
  // every other case in this file against a fixture that ignores it.
  std::vector<planar::http::fixture::captured_request> seen;
  planar::http::fixture::server                        remote([&](const planar::http::fixture::captured_request& req) {
    seen.push_back(req);
    return respond(req);
  });
  auto const                                           fx = make_fixture("headers");
  seed(fx, remote.base_url());

  REQUIRE(dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:1"}).code == 0);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].verb == "POST");
  CHECK(seen[0].target == "/rest/api/3/issue");
  CHECK(seen[0].header_value("authorization") == "Bearer tok-abc");
  CHECK(seen[0].header_value("content-type") == "application/json");
  CHECK(seen[0].header_value("accept") == "application/json");
  // The rendered entity actually reached the wire.
  CHECK(seen[0].body.contains(R"("summary":"Demo task")"));

  seen.clear();
  REQUIRE(dispatch(fx, {"ext", "create", "gh-demo", "--from", "plan:1"}).code == 0);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].target == "/repos/acme/widgets/issues");
  // The provider-specific Accept, which is the header most easily copied
  // wrongly from the sibling.
  CHECK(seen[0].header_value("accept") == "application/vnd.github+json");
  CHECK(seen[0].body.contains(R"("title":"Anchor plan")"));
}

TEST_CASE("an adapter-build failure is PROSE at exit 2, not a raw tag at exit 1", "[cmd][ext][create][factory]") {
  // THE CORRECTION. The `sync` trio renders this same failure as
  // `error: sync pull: TokenEnvVarMissing` at exit 1; `ext create` does not.
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("factory");
  seed(fx, remote.base_url());

  auto const ran = dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:1"}, "DEMO_TOKEN");
  CHECK(ran.code == 2);
  CHECK(ran.err == "error: token env var 'DEMO_TOKEN' is not set\n");
  // Explicitly NOT the sync trio's rendering, asserted rather than implied.
  CHECK_FALSE(ran.err.contains("TokenEnvVarMissing"));
  // And it refused BEFORE the remote was touched, so nothing was created.
  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("the --from ref is parsed loosely and refused late, in two exit-code buckets", "[cmd][ext][create][refusal][from]") {
  planar::http::fixture::server remote(respond);
  auto const                    fx = make_fixture("fromrefusal");
  seed(fx, remote.base_url());

  // No colon at all: refused by the PARSE, with the parse's own message.
  auto const malformed = dispatch(fx, {"ext", "create", "jira-demo", "--from", "nonsense"});
  CHECK(malformed.code == 2);
  CHECK(malformed.err == "error: invalid --from value 'nonsense'; expected kind:integer-id\n");

  // A kind that is not a table: parses fine, refused by the local READ.
  // `handlers/sync.cppm`'s parser would have produced the message above
  // instead, which is why it is not reused here.
  auto const unknown_kind = dispatch(fx, {"ext", "create", "jira-demo", "--from", "foo:1"});
  CHECK(unknown_kind.code == 2);
  CHECK(unknown_kind.err == "error: ext create: read local foo:1: InvalidInput\n");

  // A VALID `external_entity_kind` that this verb still cannot read: the
  // link table accepts seven kinds, the local read serves four.
  auto const decision = dispatch(fx, {"ext", "create", "jira-demo", "--from", "decision:1"});
  CHECK(decision.code == 2);
  CHECK(decision.err == "error: ext create: read local decision:1: InvalidInput\n");

  // Same message template, exit ONE — the row is absent rather than the kind
  // being unreadable. Collapsing the two into one exit code would be an easy
  // and invisible mistake.
  auto const missing = dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: ext create: read local task:999: NotFound\n");

  // An unknown SYSTEM is exit 1 as well, and is checked before any of these.
  auto const no_system = dispatch(fx, {"ext", "create", "no-such", "--from", "task:1"});
  CHECK(no_system.code == 1);
  CHECK(no_system.err == "error: external system 'no-such' not found\n");

  CHECK(scalar(fx, "select count(*) from external_links") == 0);
}

TEST_CASE("the remote is created BEFORE --role is validated and before the duplicate check",
          "[cmd][ext][create][defect][ordering]") {
  // TWO ORACLE DEFECTS, reproduced under D2 and pinned from the SERVER's
  // side because neither is visible in the verb's own output. Fixing either
  // later must be a deliberate, recorded divergence.
  std::vector<planar::http::fixture::captured_request> seen;
  planar::http::fixture::server                        remote([&](const planar::http::fixture::captured_request& req) {
    seen.push_back(req);
    return respond(req);
  });
  auto const                                           fx = make_fixture("ordering");
  seed(fx, remote.base_url());

  // Defect 1: a bad `--role` POSTs the ticket and THEN refuses, leaving a
  // real remote issue with no local link.
  auto const bad_role = dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:1", "--role", "bogus"});
  CHECK(bad_role.code == 2);
  CHECK(bad_role.err == "error: invalid --role 'bogus'\n");
  CHECK(seen.size() == 1);                                       // the POST happened
  CHECK(scalar(fx, "select count(*) from external_links") == 0); // the link did not

  // Defect 2: a duplicate POSTs a SECOND ticket before discovering the
  // existing link.
  seen.clear();
  REQUIRE(dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:1"}).code == 0);
  CHECK(seen.size() == 1);
  CHECK(scalar(fx, "select count(*) from external_links") == 1);

  seen.clear();
  auto const again = dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:1"});
  CHECK(again.code == 6);
  CHECK(again.err == "error: external link for task:1 on jira-demo already exists\n");
  CHECK(seen.size() == 1);                                       // a second ticket was created
  CHECK(scalar(fx, "select count(*) from external_links") == 1); // and discarded

  // The paired presence for both counts above: a refusal that fires BEFORE
  // the POST reaches the server not at all. Without this, `seen.size() == 1`
  // could not be distinguished from "every path POSTs".
  seen.clear();
  CHECK(dispatch(fx, {"ext", "create", "jira-demo", "--from", "task:999"}).code == 1);
  CHECK(seen.empty());
}
