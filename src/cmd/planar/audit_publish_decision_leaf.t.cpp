// @file audit_publish_decision_leaf.t.cpp
// @brief End-to-end tests for `planar audit publish-decision`, the leaf
// that closed the `audit` family at plan 996, task 6339.
//
// ## WHY THIS FILE ASSERTS ON THE FIXTURE SERVER'S REQUEST LOG, NOT JUST
// STDOUT
//
// Tasks 6312 and 6313 found that `ext create` POSTs a real remote ticket
// BEFORE validating `--role`, and re-POSTs on a duplicate — neither visible
// from stdout/stderr/exit-code alone, because both a correct and a broken
// implementation print the same thing on the happy path. A comment-posting
// verb has the identical exposure: does the scope guard run before any HTTP
// request, and does re-running the verb double-post? Both questions can
// only be answered by reading what the fixture server actually received,
// so every case below that matters for either question asserts the
// server's own log, not merely the process's observable output.
//
// ## THE FIXTURE'S ANSWER TO "IT ALREADY EXISTS" IS A FOURTH ONE
//
// The tree already holds three distinct answers: `ext create` re-POSTs a
// NEW remote ticket on a slug collision, `propagate-one` skips, and
// `workbench publish` refuses outright. `audit publish-decision` implements
// none of them — it has no "already exists" check at all. A decision
// comment is not an idempotent create; every invocation posts a genuinely
// new comment. "the leaf re-posts on every run" below is the case that
// pins this as the oracle's contract, not an oversight this port may
// harmonise with its siblings.
//
// ## THE TWO RAW-SQL EXCEPTIONS, AND WHY EACH IS NARROW
//
// `ext register github` always writes `base_url = "https://api.github.com"`
// — there is no `--base-url` flag on that leaf, unlike `ext register jira`.
// So routing a GitHub link through the loopback fixture server needs one
// `update external_systems set base_url = ?` after registration; everything
// else in every fixture below (`assoc`, `decision`, `ext register`, `link`)
// goes through the CLI, matching this repo's rule against hand-built
// fixture state where a verb already exists to build it.
//
// The second is `drop table external_links`, used ONLY to force
// `links_for_entity`'s SQL to fail deterministically — there is no CLI verb
// that can make a `select` statement fail on demand, so this is the only
// way to reach the propagation path a review round found this leaf had
// silently swallowed instead of propagating (see the case below named for
// it).
//
// ## PROVENANCE
//
// `audit publish-decision` was unported until this task, so there is no
// captured oracle byte string to diff against for its NEW behaviour (the
// scope guard ordering, the re-post posture). Those are asserted directly
// against `zig/src/cmd/planar/handlers/audit/publish_decision.zig`'s own
// control flow, read line by line, and against the adapter methods'
// captured byte strings in jira.t.cpp / github.t.cpp for the request shape.

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
                         std::format("planar_pubdec_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief The env every case supplies for the jira/github credential.
constexpr std::string_view k_token_env = "PUBDEC_TOKEN";

/// @brief Directly patch a registered system's `base_url`.
///
/// The ONE raw-SQL exception in this file — see the file header for why
/// `ext register github` leaves no CLI path to this column.
/// @param fx The fixture.
/// @param slug The system's slug.
/// @param base_url The new base URL.
void patch_base_url(const fixture& fx, std::string_view slug, std::string_view base_url) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("update external_systems set base_url = ? where slug = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, base_url).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Drop `external_links` so `links_for_entity` fails with
/// `query_failed` on its very next call.
///
/// The ONE other raw-SQL exception in this file — see the file header.
/// @param fx The fixture.
void drop_external_links_table(const fixture& fx) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute("drop table external_links").has_value());
}

} // namespace

TEST_CASE("publish-decision posts to a jira link and a github link, once each", "[cmd][audit][publish-decision][http]") {
  std::mutex                                           guard;
  std::vector<planar::http::fixture::captured_request> requests;

  planar::http::fixture::server remote([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const lock{guard};
    requests.push_back(req);
    return planar::http::fixture::canned_response{.status = 201, .body = R"({"id":1})", .content_type = "application/json"};
  });

  auto const fx = make_fixture("dual");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Use Postgres", "--body", "Because reasons.", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", std::string(k_token_env)})
              .code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-demo", "--project", "acme/api", "--auth-env", std::string(k_token_env)})
              .code == 0);
  patch_base_url(fx, "gh-demo", remote.base_url());
  REQUIRE(dispatch(fx, {"link", "decision:1", "--to", "jira-demo:PROJ-1"}).code == 0);
  REQUIRE(dispatch(fx, {"link", "decision:1", "--to", "gh-demo:acme/api#42"}).code == 0);

  auto const published = dispatch(fx, {"audit", "publish-decision", "1", "--json"}, {{std::string(k_token_env), "tok-abc"}});
  CHECK(published.code == 0);
  CHECK(published.out == R"({"ok":true,"decision_id":1,"comments_posted":2}
)");
  CHECK(published.err.empty());

  std::scoped_lock const lock{guard};
  REQUIRE(requests.size() == 2);
  for (auto const& req : requests) {
    CHECK(req.verb == "POST");
    CHECK(req.header_value("content-type") == std::optional<std::string>{"application/json"});
    // The rendered comment carries the decision's title and body, and the
    // footer names the entity, the session and the link — every request
    // has to carry all three regardless of which adapter sent it.
    CHECK(req.body.contains("Use Postgres"));
    CHECK(req.body.contains("Because reasons."));
    CHECK(req.body.contains("posted by planar (entity: decision:1"));
  }
  auto const jira_req = std::ranges::find_if(requests, [](auto const& r) { return r.target.contains("/rest/api/3/issue/"); });
  REQUIRE(jira_req != requests.end());
  CHECK(jira_req->target == "/rest/api/3/issue/PROJ-1/comment");

  auto const github_req = std::ranges::find_if(requests, [](auto const& r) { return r.target.contains("/repos/"); });
  REQUIRE(github_req != requests.end());
  CHECK(github_req->target == "/repos/acme/api/issues/42/comments");
}

TEST_CASE("publish-decision refuses a cross-scope decision BEFORE any request is sent", "[cmd][audit][publish-decision][http]") {
  std::mutex  remote_guard;
  std::size_t requests_seen = 0;

  planar::http::fixture::server remote([&](const planar::http::fixture::captured_request&) {
    std::scoped_lock const lock{remote_guard};
    ++requests_seen;
    return planar::http::fixture::canned_response{.status = 201, .body = R"({"id":1})", .content_type = "application/json"};
  });

  auto const fx = make_fixture("scopeguard");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  // "acme" names a real association, but its member project is a DIFFERENT
  // directory from the fixture's cwd (`fx.root / "proj"`), so the operator's
  // write scope resolves to global while the decision's is `acme` — a
  // genuine cross-scope write, not a typo'd slug.
  REQUIRE(dispatch(fx, {"assoc", "create", "acme"}).code == 0);
  std::error_code ec;
  std::filesystem::create_directories(fx.root / "other", ec);
  REQUIRE(dispatch(fx, {"assoc", "add", "acme", (fx.root / "other").string()}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Scoped decision", "--body", "Body.", "--scope", "acme", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", std::string(k_token_env)})
              .code == 0);
  REQUIRE(dispatch(fx, {"link", "decision:1", "--to", "jira-demo:PROJ-1"}).code == 0);

  auto const refused = dispatch(fx, {"audit", "publish-decision", "1"}, {{std::string(k_token_env), "tok-abc"}});
  CHECK(refused.code == 5);
  CHECK(refused.err == "error: decision 1 belongs to a different scope\n");
  CHECK(refused.out.empty());

  // The load-bearing assertion: the guard runs BEFORE the comment is built
  // or any adapter is touched, so the fixture server that would happily
  // accept the POST never sees one.
  std::scoped_lock const lock{remote_guard};
  CHECK(requests_seen == 0);
}

TEST_CASE("publish-decision re-posts on every invocation; there is no de-dup", "[cmd][audit][publish-decision][http]") {
  std::mutex                                           guard;
  std::vector<planar::http::fixture::captured_request> requests;

  planar::http::fixture::server remote([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const lock{guard};
    requests.push_back(req);
    return planar::http::fixture::canned_response{.status = 201, .body = R"({"id":1})", .content_type = "application/json"};
  });

  auto const fx = make_fixture("repost");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Repostable", "--body", "Body.", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", std::string(k_token_env)})
              .code == 0);
  REQUIRE(dispatch(fx, {"link", "decision:1", "--to", "jira-demo:PROJ-1"}).code == 0);

  auto const first = dispatch(fx, {"audit", "publish-decision", "1", "--json"}, {{std::string(k_token_env), "tok-abc"}});
  CHECK(first.code == 0);
  CHECK(first.out == R"({"ok":true,"decision_id":1,"comments_posted":1}
)");

  auto const second = dispatch(fx, {"audit", "publish-decision", "1", "--json"}, {{std::string(k_token_env), "tok-abc"}});
  CHECK(second.code == 0);
  // Same link, same decision, same count — NOT zero and NOT a refusal. A
  // version that tracked "already posted" per link would report 0 here.
  CHECK(second.out == R"({"ok":true,"decision_id":1,"comments_posted":1}
)");

  std::scoped_lock const lock{guard};
  CHECK(requests.size() == 2);
}

TEST_CASE("publish-decision posts to a target reached only via entity_links", "[cmd][audit][publish-decision][http]") {
  std::mutex                                           guard;
  std::vector<planar::http::fixture::captured_request> requests;

  planar::http::fixture::server remote([&](const planar::http::fixture::captured_request& req) {
    std::scoped_lock const lock{guard};
    requests.push_back(req);
    return planar::http::fixture::canned_response{.status = 201, .body = R"({"id":1})", .content_type = "application/json"};
  });

  auto const fx = make_fixture("transitive");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Implement it", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Adopt approach", "--body", "Body.", "--json"}).code == 0);
  // Linked FROM the decision TO the task — publish-decision widens through
  // this edge even though the task carries no direct external link of the
  // decision's own kind.
  REQUIRE(dispatch(fx, {"links", "add", "decision:1", "task:1", "--relationship", "cites"}).code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", remote.base_url(), "--project", "DEMO",
                        "--auth-env", std::string(k_token_env)})
              .code == 0);
  // The link sits on the TASK, not the decision.
  REQUIRE(dispatch(fx, {"link", "task:1", "--to", "jira-demo:PROJ-1"}).code == 0);

  auto const published = dispatch(fx, {"audit", "publish-decision", "1", "--json"}, {{std::string(k_token_env), "tok-abc"}});
  CHECK(published.code == 0);
  CHECK(published.out == R"({"ok":true,"decision_id":1,"comments_posted":1}
)");

  std::scoped_lock const lock{guard};
  REQUIRE(requests.size() == 1);
  CHECK(requests.front().body.contains("posted by planar (entity: task:1"));
}

TEST_CASE("publish-decision refuses an unknown decision id at exit 1", "[cmd][audit][publish-decision]") {
  auto const fx = make_fixture("notfound");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  auto const missing = dispatch(fx, {"audit", "publish-decision", "999"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: decision 999 not found\n");
}

TEST_CASE("publish-decision succeeds at zero comments when the decision has no links", "[cmd][audit][publish-decision]") {
  auto const fx = make_fixture("nolinks");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Lonely decision", "--body", "Body.", "--json"}).code == 0);

  auto const published = dispatch(fx, {"audit", "publish-decision", "1"});
  CHECK(published.code == 0);
  CHECK(published.out == "decision 1 published: 0 comment(s) posted\n");
}

TEST_CASE("publish-decision PROPAGATES a direct-target link-read failure, not swallows it", "[cmd][audit][publish-decision]") {
  // A review round found this leaf's first draft answered `if (!links) {
  // return; }` for the `links_for_entity` read `publish_target` opens
  // with — silently reporting `0 comment(s) posted` at exit 0 on a SQL
  // failure the oracle treats as fatal
  // (zig/src/cmd/planar/handlers/audit/publish_decision.zig's bare `try`,
  // caught at the call site with `exit.die`). This case is the fix's own
  // regression guard: it forces that exact read to fail and asserts the
  // verb aborts naming it, not that it degrades to a plausible-looking
  // success.
  auto const fx = make_fixture("linkreadfail");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Doomed decision", "--body", "Body.", "--json"}).code == 0);
  drop_external_links_table(fx);

  auto const published = dispatch(fx, {"audit", "publish-decision", "1"});
  CHECK(published.code == 1);
  CHECK(published.err == "error: audit publish-decision: direct target: QueryFailed\n");
  CHECK(published.out.empty());
}
