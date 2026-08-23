// @file handlers.t.cpp
// @brief In-process tests for `planar-agent`'s ported handlers and its
// exit-code policy (plan 996, task 6107).
//
// These run the REAL handlers through the REAL dispatch with no subprocess,
// which is what the explicit-`context` design buys: a case can assert
// `context::db_opened()` — whether a verb touched SQLite at all — which no
// shelled binary can expose.
//
// HOME / DB SAFETY. Every case builds its own environment map over a unique
// scratch root; nothing here calls std::getenv.
//
// ORACLE PROVENANCE. Every expected string is a verbatim transcription of
// bytes `zig/zig-out/bin/planar-agent` wrote under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/PLANAR_LOCAL_HOME:
//
//   $Z version         exit 0, stdout b'planar-agent dev dev zig 0.16.0\n'
//   $Z nosuchverb      exit 1  <-- ONE, not the operator binary's two
//                      stdout b'error: unknown subcommand (got nosuchverb) [in: planar-agent]\n'
//                      stderr b'error: UnknownSubcommand\n'
//   $Z version --badflag
//                      exit 1
//                      stdout b'error: unknown flag (got --badflag)\n'
//                      stderr b'error: UnknownFlag\n'
//   $Z --help          exit 0, root help page on stdout
//   $Z (bare)          exit 0, same root help page
//   $Z schema          exit 0, single-line JSON catalog + '\n'
//
// ## Break-probes run against this file
//
//   - Swapped `run`'s parse-error mapping to
//     `cli::exit_code_for_parse_error_planar_binary` (the operator
//     binary's policy — the exact silent mistake task 6066 renamed that
//     function to prevent) -> `maps a parse failure to exit 1, not the
//     operator binary's 2` FAILS with 2 (both the unknown-verb and the
//     unknown-flag assertions). Restored -> green.
//   - Changed `render_version_text("planar-agent", ...)` back to the
//     single-argument overload -> `version names THIS binary` FAILS.
//     Restored -> green.
//   - Dropped the `'\n'` the schema handler appends -> `schema appends the
//     terminator its renderer omits` FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cli;
import planar.db;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.tree;

namespace {

using planar::cmd::agent::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A fixture root plus the environment every case dispatches under.
struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar_cmd_agent_h_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
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
  std::vector<std::string> argv{"planar-agent"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::agent::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::agent::root_command();
  auto const         table = planar::cmd::agent::handlers(tree);
  int const          code  = planar::cmd::agent::run(ctx, tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

} // namespace

TEST_CASE("planar-agent version names THIS binary and opens no database", "[cmd][agent][handlers]") {
  auto const fx  = make_fixture("version");
  auto const got = dispatch(fx, {"version"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);

  // The prefix is the contract zig/src/cmd/planar-watch/handlers/version.zig
  // states in prose: operators grep on it to tell the binaries apart. A
  // line starting "planar " here would be indistinguishable from the
  // operator binary's — which is exactly what the single-argument
  // `render_version_text` overload would have produced.
  CHECK(got.out.starts_with("planar-agent dev dev cxx "));
  CHECK_FALSE(got.out.starts_with("planar dev"));
  CHECK(got.out.ends_with("\n"));

  // FIELD COUNT DOES NOT MATCH, and this assertion records that rather
  // than asserting the claim. `planar.cli.version`'s module header says the
  // `cxx` divergence "preserves the field COUNT — a script splitting on
  // whitespace still finds five tokens". It does not:
  // `compiler_version_string()` returns `Clang 22.1.8`, which itself
  // contains a space, so the line splits into SIX tokens where the oracle's
  // `planar-agent dev dev zig 0.16.0` splits into five. Task 6106 found
  // this on the operator binary (src/cmd/planar/handlers.t.cpp) and filed
  // it against layer 1; this case confirms it is not binary-specific.
  // Pinned at the ACTUAL value so a later fix in `planar.cli.version` shows
  // up here as a failing test rather than passing silently.
  auto const fields = std::ranges::count(got.out, ' ') + 1;
  CHECK(fields == 6);
}

TEST_CASE("planar-agent schema appends the terminator its renderer omits", "[cmd][agent][handlers]") {
  auto const fx  = make_fixture("schema");
  auto const got = dispatch(fx, {"schema"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);

  // `schema_json`'s @return documents "no trailing newline — callers …
  // append one at the write site". So exactly ONE newline, at the end, and
  // the payload is a single line (the Zig emitter's flat catalog is not
  // pretty-printed).
  REQUIRE(got.out.ends_with("\n"));
  CHECK(got.out.find('\n') == got.out.size() - 1);

  // The catalog describes THIS tree: its root name, and only the ported
  // verbs. A catalog naming a verb the binary cannot run would be worse
  // than no catalog — tools/cli_usage_lint.zig validates authored surfaces
  // against exactly this output.
  CHECK(got.out.contains("\"root\":\"planar-agent\""));
  CHECK(got.out.contains("\"planar-agent version\""));
  CHECK(got.out.contains("\"planar-agent schema\""));
  // Task 6038 landed the claim ritual, so these ARE in the catalog now.
  CHECK(got.out.contains("\"planar-agent pull\""));
  CHECK(got.out.contains("\"planar-agent complete\""));
  CHECK(got.out.contains("\"planar-agent action start\""));
  // ...and the four still-deferred verbs are NOT. A catalog naming a verb
  // the binary cannot run would be worse than no catalog.
  CHECK_FALSE(got.out.contains("\"planar-agent ingest\""));
  CHECK_FALSE(got.out.contains("\"planar-agent run start\""));
  CHECK_FALSE(got.out.contains("\"planar-agent dispatch preview\""));
  CHECK_FALSE(got.out.contains("\"planar-agent context add\""));
}

TEST_CASE("planar-agent maps a parse failure to exit 1, not the operator binary's 2", "[cmd][agent][handlers][exitcode]") {
  auto const fx = make_fixture("parse");

  // THE divergence this binary exists to get right. Oracle-captured:
  // `planar-agent nosuchverb` exits 1 while `planar nosuchverb` exits 2,
  // from identical argv shapes.
  auto const unknown_verb = dispatch(fx, {"nosuchverb"});
  CHECK(unknown_verb.code == 1);
  // Both streams, and that is the oracle's shape too — a reasonable person
  // would have put the parse error on stderr alone and been wrong.
  CHECK(unknown_verb.out == "error: unknown subcommand (got nosuchverb) [in: planar-agent]\n");
  CHECK(unknown_verb.err == "error: UnknownSubcommand\n");
  CHECK_FALSE(unknown_verb.db_open);

  auto const unknown_flag = dispatch(fx, {"version", "--badflag"});
  CHECK(unknown_flag.code == 1);
  CHECK(unknown_flag.out == "error: unknown flag (got --badflag)\n");
  CHECK(unknown_flag.err == "error: UnknownFlag\n");
}

TEST_CASE("planar-agent help paths exit 0 and open no database", "[cmd][agent][handlers]") {
  auto const fx = make_fixture("help");

  auto const explicit_help = dispatch(fx, {"--help"});
  CHECK(explicit_help.code == 0);
  CHECK(explicit_help.err.empty());
  CHECK_FALSE(explicit_help.db_open);
  CHECK(explicit_help.out.starts_with("planar-agent\n"));
  // The desc renders INDENTED, which is `planar.cli.help`'s desc branch —
  // this root deliberately carries no long_desc. See tree.cpp.
  CHECK(
      explicit_help.out.contains("\n  Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).\n"));

  // A bare invocation is the same page, exit 0 — matching the oracle.
  auto const bare = dispatch(fx, {});
  CHECK(bare.code == 0);
  CHECK(bare.out == explicit_help.out);

  auto const leaf_help = dispatch(fx, {"version", "--help"});
  CHECK(leaf_help.code == 0);
  // Leaf help pages ARE oracle-comparable byte for byte: a leaf's page is
  // derived entirely from its own node, so a leaf ported at all is ported
  // completely.
  CHECK(leaf_help.out == "version\n"
                         "\n"
                         "  Print the planar-agent version, commit, and zig runtime.\n"
                         "\n"
                         "USAGE:\n"
                         "  version\n");
}

TEST_CASE("planar-agent leaf help for schema matches the oracle byte for byte", "[cmd][agent][handlers]") {
  auto const fx  = make_fixture("schemahelp");
  auto const got = dispatch(fx, {"schema", "--help"});
  CHECK(got.code == 0);
  CHECK(got.out == "schema\n"
                   "\n"
                   "  Print the full command tree as a JSON catalog (flags, aliases, positionals).\n"
                   "\n"
                   "USAGE:\n"
                   "  schema\n");
}
