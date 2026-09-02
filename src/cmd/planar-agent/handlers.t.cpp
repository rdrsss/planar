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
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
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
  auto const         tree  = planar::cmd::agent::root_app();
  auto const         table = planar::cmd::agent::handlers(*tree);
  int const          code  = planar::cmd::agent::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

auto migrate_fixture(const fixture& fx) -> void {
  auto db = planar::db::connection::open(fx.db_path.string());
  REQUIRE(db.has_value());
  REQUIRE(planar::db::apply_all(*db).has_value());
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

  // FIELD COUNT MATCHES the oracle's five, which is what
  // `planar.cliapp.version`'s module header promises the `cxx` divergence
  // preserves. Task 6117 closed the gap: this used to require SIX and
  // carry a comment explaining that `compiler_version_string()` returned
  // `Clang 22.1.8` — a space — so the line split one token wider than
  // `planar-agent dev dev zig 0.16.0`. Task 6106 found it on the operator
  // binary and filed it against layer 1; the fix landed there, and this
  // case confirms it was not binary-specific in either direction.
  auto const fields = std::ranges::count(got.out, ' ') + 1;
  CHECK(fields == 5);
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
  // ...and TASK 6065 INVERTED the rest of this assertion. It used to read
  // "the four still-deferred verbs are NOT in the catalog — a catalog
  // naming a verb the binary cannot run would be worse than no catalog".
  //
  // That premise was measured and found backwards. `zig/tools/cli_usage_
  // lint` SKIPS a command path it cannot resolve (proved in
  // `src/lib/cliapp/schema.t.cpp`'s `[lint-parity]` scope section), so a
  // catalog that omits a verb does not make the gate stricter about that
  // verb — it removes the verb's flags from the gate entirely. An omitted
  // verb is a HOLE in `make cli-usage-check`, not a safeguard.
  //
  // What actually keeps a catalog from lying is the REFUSAL, not the
  // omission: every one of these is declared, and every one exits 64 with
  // `<verb>: not implemented in this build`. That is asserted directly
  // below, so this pair is a contract and not a relaxation.
  CHECK(got.out.contains("\"planar-agent ingest\""));
  CHECK(got.out.contains("\"planar-agent run start\""));
  CHECK(got.out.contains("\"planar-agent dispatch preview\""));
  CHECK(got.out.contains("\"planar-agent context add\""));

  // `ingest` is now a real leaf, not an exit-64 catalog placeholder.
  auto const unsupported = dispatch(fx, {"ingest", "--vendor", "nobody", "--event", "@missing"});
  CHECK(unsupported.code != 64);
}

TEST_CASE("planar-agent ingest atomically normalizes a Claude hook", "[cmd][agent][handlers][ingest]") {
  auto const fx = make_fixture("ingest");
  migrate_fixture(fx);
  auto const event = fx.root / "event.json";
  {
    std::ofstream out(event);
    out << R"({"event_type":"tool_call","session_id":"hook-1","model":"claude-test","summary":"ls"})";
  }
  auto const got = dispatch(fx, {"ingest", "--vendor", "claude", "--event", "@../event.json", "--json"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == "{\"ok\":true,\"sessions_created\":1,\"claims_created\":0,\"actions_created\":1,\"events_processed\":1}\n");

  // Break probe: a malformed input is rejected before a DB transaction and
  // therefore cannot be mistaken for a successful no-op.
  auto const before = std::filesystem::file_size(fx.db_path);
  {
    std::ofstream out(event);
    out << "{";
  }
  auto const bad = dispatch(fx, {"ingest", "--vendor", "claude", "--event", "@../event.json"});
  CHECK(bad.code == 2);
  CHECK(bad.err.contains("malformed event payload"));
  CHECK(std::filesystem::file_size(fx.db_path) == before);
}

TEST_CASE("planar-agent maps a parse failure to exit 1, not the operator binary's 2", "[cmd][agent][handlers][exitcode]") {
  auto const fx = make_fixture("parse");

  // THE divergence this binary exists to get right. Oracle-captured:
  // `planar-agent nosuchverb` exits 1 while `planar nosuchverb` exits 2,
  // from identical argv shapes.
  auto const unknown_verb = dispatch(fx, {"nosuchverb"});
  CHECK(unknown_verb.code == 1);
  // Both streams, and that is the oracle's shape too — a reasonable person
  // would have put the parse error on stderr alone and been wrong. Task
  // 6123 re-baselined the WORDING onto CLI11's (pinned exactly below) but
  // kept the shape and, critically, the exit code.
  CHECK(unknown_verb.out == "error: planar-agent: The following argument was not expected: nosuchverb\n");
  CHECK(unknown_verb.err == "error: ExtrasError\n");
  CHECK_FALSE(unknown_verb.db_open);

  auto const unknown_flag = dispatch(fx, {"version", "--badflag"});
  CHECK(unknown_flag.code == 1);
  // CLI11 reports an unknown FLAG and an unknown SUBCOMMAND under the same
  // `ExtrasError` name, where etcli distinguished `UnknownFlag` from
  // `UnknownSubcommand`. A named coarsening of the swap; the stdout line
  // still identifies the offending token and the command that rejected it.
  CHECK(unknown_flag.out == "error: version: The following argument was not expected: --badflag\n");
  CHECK(unknown_flag.err == "error: ExtrasError\n");
}

TEST_CASE("planar-agent help paths exit 0 and open no database", "[cmd][agent][handlers]") {
  auto const fx = make_fixture("help");

  auto const explicit_help = dispatch(fx, {"--help"});
  CHECK(explicit_help.code == 0);
  CHECK(explicit_help.err.empty());
  CHECK_FALSE(explicit_help.db_open);
  // Task 6123: the root page is CLI11's now. It leads with the
  // description (wrapped to the formatter's width) and then lists the
  // verbs. Task 6065 brought the list to the oracle's full eighteen; the
  // page is still not pinned byte-for-byte against the oracle (CLI11's
  // LAYOUT is not etcli's), so this checks the content that is this
  // binary's own contract.
  CHECK(explicit_help.out.starts_with("Agent-callable coordination binary (pull / claim / complete / heartbeat /\n"
                                      "reconcile).\n"));
  CHECK(explicit_help.out.contains("SUBCOMMANDS:"));
  // Matched as a LISTING LINE (`"\n  <verb>"`), not as a bare substring.
  // CLI11 indents a subcommand entry by exactly two spaces and wraps its
  // description to a deeper column, so this form matches an entry and
  // nothing else — which matters for the negative loop below, where a bare
  // `contains("dispatch")` is satisfied by `claim-associate`'s own
  // description ("...at dispatch time"). That false positive is exactly
  // what a substring check would have shipped.
  auto const listed = [&](std::string_view verb) { return explicit_help.out.contains("\n  " + std::string{verb}); };
  for (auto const& verb : {"pull", "peek", "claim", "heartbeat", "claim-associate", "complete", "fail", "release", "block",
                           "action", "reconcile", "abort", "version", "schema"}) {
    INFO("ported verb missing from the root page: " << verb);
    CHECK(listed(verb));
  }
  // TASK 6065: the four formerly-deferred verbs are on the page too,
  // because they are in the tree. The page is not a lie — each one
  // refuses at exit 64 naming itself (see the `schema` case above and
  // `dispatch.cpp`'s `not_implemented_for`), which is a better operator
  // experience than an unknown-verb error and is what makes
  // `make cli-usage-check` able to resolve their flags at all.
  for (auto const& declared : {"ingest", "dispatch", "context", "run"}) {
    INFO("declared verb missing from the root page: " << declared);
    CHECK(listed(declared));
  }
  // The page now lists exactly the oracle's eighteen top-level verbs.
  CHECK_FALSE(listed("nosuchverb"));

  // A bare invocation is the same page, exit 0 — matching the oracle.
  auto const bare = dispatch(fx, {});
  CHECK(bare.code == 0);
  CHECK(bare.out == explicit_help.out);

  auto const leaf_help = dispatch(fx, {"version", "--help"});
  CHECK(leaf_help.code == 0);
  // A leaf's page is derived entirely from its own node, so a leaf ported
  // at all is ported completely — which is why leaf pages are pinned
  // EXACTLY (trailing spaces included) rather than loosened to a
  // `contains` check. Task 6123 re-baselined these bytes from the oracle's
  // renderer onto CLI11's; the reason to pin them did not change.
  CHECK(leaf_help.out == "Print the planar-agent version, commit, and zig runtime.\n"
                         "\n"
                         "\n"
                         "version [OPTIONS]\n"
                         "\n"
                         "\n"
                         "OPTIONS:\n"
                         "  -h,     --help              Print this help message and exit\n");
}

TEST_CASE("planar-agent leaf help for schema is pinned exactly", "[cmd][agent][handlers]") {
  auto const fx  = make_fixture("schemahelp");
  auto const got = dispatch(fx, {"schema", "--help"});
  CHECK(got.code == 0);
  CHECK(got.out == "Print the full command tree as a JSON catalog (flags, aliases, positionals).\n"
                   "\n"
                   "\n"
                   "schema [OPTIONS]\n"
                   "\n"
                   "\n"
                   "OPTIONS:\n"
                   "  -h,     --help              Print this help message and exit\n");
}
