// @file ext_leaves.t.cpp
// @brief End-to-end tests for the `planar ext test` leaf wired by plan 996,
// task 6258.
//
// ## Why the FACTORY cases are not here
//
// They are in `ext_factory.t.cpp`, and the split is forced rather than
// stylistic — that file's header gives the full account. Short version: the
// factory module opens `namespace planar::cmd::ext::handlers`, the dispatch
// helper below calls the FUNCTION `planar::cmd::ext::handlers(*tree)`, and with
// both in scope the qualified name is ambiguous and the TU does not compile.
// `init.t.cpp` hit the same wall first and states the rule.
//
// What is here is what only the whole verb can show: that the context's env
// lookup actually reaches the factory, and that the two failure modes of
// this one verb carry DIFFERENT exit codes.
//
// ## THIS LEAF REACHES NO NETWORK, WHICH IS WHY THERE IS NO FIXTURE SERVER
//
// The oracle's own handler comment says it does not probe the remote, and
// its `probeOk` only asks whether construction returned an adapter. So the
// transport is built and immediately dropped, and there is nothing for an
// in-process HTTP server to serve. A fixture server here would be
// scaffolding that proves nothing rather than extra rigour. The registered
// `base_url` is `https://example.invalid` — a reserved TLD that cannot
// resolve — so even a regression that started probing would fail loudly
// rather than reach a real host.
//
// ## TWO FAILURE MODES, TWO EXIT CODES
//
// An unknown slug is exit 1; a credential refusal is exit 2. Both are
// failures of the same verb, and a port that collapsed them to one code
// would still pass either case read alone. They are asserted together.
//
// Captured from `zig/zig-out/bin/planar` in a 22-case differential that
// matched byte for byte on stdout, stderr AND exit code.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.dispatch;
import planar.cmd.planar_ext.main;

namespace {

using planar::cmd::ext::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
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
                         std::format("planar_ext_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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

/// @brief Migrate the fixture database directly.
///
/// `planar-ext` has no `init` verb — that verb, and migration, stay on
/// `planar` (`planar.cmd.planar_ext.context`'s header). So this test binary
/// cannot bootstrap the schema through `dispatch`; it applies the full
/// migration chain the same way `capability.t.cpp` does.
/// @param fx The fixture whose `db_path` gets migrated.
void migrate_fixture(const fixture& fx) {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn));
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

  auto const         cwd = fx.root / "proj";
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::ext::map_env(vars), cwd, std::make_shared<planar::cmd::ext::database>(fx.db_path, err), out, err};
  auto const         tree  = planar::cmd::ext::root_app();
  auto const         table = planar::cmd::ext::handlers(*tree);
  int const          code  = planar::cmd::ext::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

/// @brief Register the systems every case below reads.
///
/// Through the CLI rather than raw SQL, so the rows are the ones an operator
/// would actually have. The result is CHECKed and then READ BACK: a fixture
/// whose seeding silently failed would leave every case asserting against
/// "system not found" and passing for the wrong reason. That is not
/// hypothetical — `assoc add <slug> .` storing a literal `.` did exactly
/// this to a whole scope-resolver suite (task 6256).
/// @param fx The fixture.
void seed(const fixture& fx) {
  migrate_fixture(fx);
  REQUIRE(dispatch(fx, {"ext", "register", "jira", "jira-demo", "--base-url", "https://example.invalid", "--project", "DEMO",
                        "--auth-env", "DEMO_TOKEN"})
              .code == 0);
  REQUIRE(dispatch(fx, {"ext", "register", "github", "gh-demo", "--project", "acme/repo", "--auth-env", "GH_TOKEN"}).code == 0);
  auto const listed = dispatch(fx, {"ext", "list", "--json"});
  REQUIRE(listed.code == 0);
  REQUIRE(listed.out.contains("jira-demo"));
  REQUIRE(listed.out.contains("gh-demo"));
}

} // namespace

TEST_CASE("ext test renders the wired line in both modes and opens SQLite", "[cmd][ext][test][render]") {
  auto const fx = make_fixture("render");
  seed(fx);

  auto const text = dispatch(fx, {"ext", "test", "jira-demo"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(text.code == 0);
  // TWO spaces before the parenthesis.
  CHECK(text.out == "jira-demo: ok  (adapter wired)\n");
  CHECK(text.err.empty());
  CHECK(text.db_open);

  auto const json = dispatch(fx, {"ext", "test", "jira-demo", "--json"}, {{"DEMO_TOKEN", "tok-abc"}});
  CHECK(json.code == 0);
  CHECK(json.out == "{\"slug\":\"jira-demo\",\"ok\":true}\n");
}

TEST_CASE("a github row wires through the same leaf", "[cmd][ext][test][github]") {
  auto const fx = make_fixture("github");
  seed(fx);

  auto const wired = dispatch(fx, {"ext", "test", "gh-demo"}, {{"GH_TOKEN", "ghtok"}});
  CHECK(wired.code == 0);
  CHECK(wired.out == "gh-demo: ok  (adapter wired)\n");
}

TEST_CASE("an unknown slug is exit 1, while a credential refusal is exit 2", "[cmd][ext][test][exitcodes]") {
  auto const fx = make_fixture("codes");
  seed(fx);

  auto const unknown = dispatch(fx, {"ext", "test", "no-such-system"});
  CHECK(unknown.code == 1);
  CHECK(unknown.err == "error: external system 'no-such-system' not found\n");

  auto const refused = dispatch(fx, {"ext", "test", "jira-demo"});
  CHECK(refused.code == 2);
  CHECK(refused.err == "error: token env var 'DEMO_TOKEN' is not set\n");
}

TEST_CASE("the refusal stays plain text under --json, with an EMPTY stdout", "[cmd][ext][test][json][refusal]") {
  auto const fx = make_fixture("jsonrefusal");
  seed(fx);

  // `--json` does NOT make the error path emit JSON, and stdout stays empty
  // — a consumer parsing stdout gets zero bytes, not `{"ok":false}`.
  auto const refused = dispatch(fx, {"ext", "test", "jira-demo", "--json"});
  CHECK(refused.code == 2);
  CHECK(refused.out.empty());
  CHECK(refused.err == "error: token env var 'DEMO_TOKEN' is not set\n");
}

TEST_CASE("an empty DEMO_TOKEN wires an adapter through the real dispatch path", "[cmd][ext][test][empty]") {
  auto const fx = make_fixture("emptyenv");
  seed(fx);

  // `ext_factory.t.cpp` asserts this at the factory; this proves it survives
  // the whole handler, including the env lookup the context owns. The two
  // are not redundant: a handler that pre-validated the variable before
  // calling the factory would pass the factory case and fail this one.
  auto const wired = dispatch(fx, {"ext", "test", "jira-demo"}, {{"DEMO_TOKEN", ""}});
  CHECK(wired.code == 0);
  CHECK(wired.out == "jira-demo: ok  (adapter wired)\n");
}

TEST_CASE("ext list --json OMITS a null base_url and default_project entirely", "[cmd][ext][list][parity][terminator]") {
  // Moved from `planar`'s `handlers.t.cpp` at plan 996, task 6419 — `ext
  // list` now lives here.
  //
  // Not reachable through `ext register`: both register helpers always set
  // both columns, so this branch never fires under the CLI and a break-probe
  // that replaced it with `value_or("")` survived the whole parity suite. The
  // branch is still real — `system::register_system` takes both as optionals,
  // and a future `linear` / `gitlab-issues` registration need not set a base
  // URL — and the Zig renderer branches on the optional rather than
  // serializing it, so a `"base_url":null` would be a divergence the moment
  // such a row exists. Seeded here with raw SQL, which is the only way in —
  // and the SAME row was seeded into a scratch database and read back through
  // the ORACLE, so both expectations below are CAPTURED bytes:
  //
  //   $Z ext list --json
  //     b'{"id":1,"kind":"linear","slug":"lin","auth_method":"token-env",
  //       "created_at":"..."}\n'
  //   $Z ext list
  //     b'slug                  kind              base-url
  //       project\nlin                   linear
  //                       \n'
  //
  // Note the TEXT form's TRAILING WHITESPACE. The empty project column is
  // last and unpadded, but the empty base-url column before it is padded to
  // 36, so the line ends in spaces. Trimming it would be a divergence.
  auto const fx = make_fixture("extnull");
  migrate_fixture(fx);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("insert into external_systems (kind, slug, base_url, default_project, auth_method, auth_ref) "
                          "values ('linear', 'lin', null, null, 'token-env', 'TOK')")
                .has_value());
  }

  auto const got = dispatch(fx, {"ext", "list", "--json"});
  CHECK(got.code == 0);
  // Neither key appears at all, and the key ORDER around the gap is
  // unchanged: id, kind, slug, [base_url], [default_project], auth_method,
  // created_at.
  CHECK(got.out.find("base_url") == std::string::npos);
  CHECK(got.out.find("default_project") == std::string::npos);
  CHECK(got.out.find("null") == std::string::npos);
  CHECK(got.out.starts_with(R"({"id":1,"kind":"linear","slug":"lin","auth_method":"token-env","created_at":")"));
  CHECK(got.out.ends_with("\"}\n"));

  // And the TEXT renderer prints an empty cell rather than the word `null`,
  // padded to the same width.
  auto const text = dispatch(fx, {"ext", "list"});
  CHECK(text.code == 0);
  CHECK(text.out == "slug                  kind              base-url                              project\n"
                    "lin                   linear                                                  \n");
}
