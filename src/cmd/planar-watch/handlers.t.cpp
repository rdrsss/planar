// @file handlers.t.cpp
// @brief In-process tests for `planar-watch`'s ported handlers (plan 996,
// task 6107).
//
// HOME / DB SAFETY. Every case builds its own environment map over a unique
// scratch root; nothing here calls std::getenv.
//
// ORACLE PROVENANCE. Every expected string is a verbatim transcription of
// bytes `zig/zig-out/bin/planar-watch` wrote under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH/PLANAR_LOCAL_HOME:
//
//   $Z version                 exit 0, b'planar-watch dev dev zig 0.16.0\n'
//   $Z nosuchverb              exit 1
//                              stdout b'error: unknown subcommand (got nosuchverb) [in: planar-watch]\n'
//                              stderr b'error: UnknownSubcommand\n'
//   $Z completion badshell     exit 2, stdout b''
//                              stderr b"error: unsupported shell 'badshell'; supported: bash, zsh, fish\n"
//   $Z completion              exit 1
//                              stdout b'error: required positional missing: <shell>\n'
//                              stderr b'error: MissingRequiredPositional\n'
//   $Z completion --help       exit 0
//                              b'completion\n\n  Generate the autocompletion script for the specified shell.\n\n'
//                              b'USAGE:\n  completion <shell>\n\n'
//                              b'POSITIONAL ARGUMENTS:\n  <shell>         (string) — Shell: bash, zsh, or fish\n'
//   $Z --help                  exit 0, root page with the long_desc FLUSH LEFT
//
// THE TWO EXIT CODES ON ONE VERB are the discrimination worth keeping: a
// shell value the parser accepted but the handler rejects is 2
// (invalid_input); a positional the parser never saw is 1 (parse error,
// this binary's policy). A port that collapsed this binary's exit mapping
// to a single code — the easiest way to get it wrong — cannot pass both.
//
// ## Break-probes run against this file
//
//   - Swapped `run`'s parse-error mapping to the operator binary's
//     `exit_code_for_parse_error_planar_binary` -> `a bad shell exits 2, a
//     missing positional exits 1` FAILS on the missing-positional half
//     (2 where 1 is required) while the bad-shell half still passes.
//     Restored -> green.
//   - Changed the invalid-shell error kind from `invalid_input` to
//     `generic_failure` -> the SAME case fails on the OTHER half (1 where
//     2 is required). Restored -> green. Two mutations in opposite
//     directions, each caught by a different half, is what makes the pair
//     worth keeping in one case.
//   - Changed `render_version_text("planar-watch", ...)` to the
//     single-argument overload -> `version names THIS binary` FAILS.
//     Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.tree;

namespace {

using planar::cmd::watch::context;

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
                    std::format("planar_cmd_watch_h_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
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
  std::vector<std::string> argv{"planar-watch"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::watch::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::watch::root_app();
  auto const         table = planar::cmd::watch::handlers(*tree);
  int const          code  = planar::cmd::watch::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

} // namespace

TEST_CASE("planar-watch version names THIS binary and opens no database", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("version");
  auto const got = dispatch(fx, {"version"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // On the read-only binary this negative matters more than elsewhere: the
  // cheapest way to break "a viewer never creates state" is an eager open
  // at startup, and that would be invisible in stdout.
  CHECK_FALSE(got.db_open);

  CHECK(got.out.starts_with("planar-watch dev dev cxx "));
  CHECK_FALSE(got.out.starts_with("planar dev"));
  CHECK(got.out.ends_with("\n"));
}

TEST_CASE("planar-watch completion emits a distinct script per shell and opens no database", "[cmd][watch][handlers]") {
  auto const fx = make_fixture("completion");

  auto const bash = dispatch(fx, {"completion", "bash"});
  auto const zsh  = dispatch(fx, {"completion", "zsh"});
  auto const fish = dispatch(fx, {"completion", "fish"});

  for (auto const* got : {&bash, &zsh, &fish}) {
    CHECK(got->code == 0);
    CHECK(got->err.empty());
    CHECK_FALSE(got->db_open);
    CHECK_FALSE(got->out.empty());
  }

  // Three DIFFERENT scripts, not one script emitted three times — the
  // failure mode of a shell switch that fell through.
  CHECK(bash.out != zsh.out);
  CHECK(zsh.out != fish.out);
  CHECK(bash.out != fish.out);

  // Generated from THIS binary's tree, per
  // zig/src/cmd/planar-watch/handlers/completion.zig's "per-binary
  // completions without sharing state between the command surfaces".
  CHECK(bash.out.contains("planar-watch"));
  CHECK(bash.out.contains("completion"));

  // NOT oracle-byte-compared: `planar.cliapp.completion` defers flag-VALUE
  // completion (see its module header), so the emitted script differs from
  // the reference's. Nothing here claims otherwise.
}

TEST_CASE("planar-watch completion: a bad shell exits 2, a missing positional exits 1", "[cmd][watch][handlers][exitcode]") {
  auto const fx = make_fixture("shellerr");

  // Handler-level refusal: invalid_input -> 2. The body is the oracle's,
  // verbatim.
  auto const bad = dispatch(fx, {"completion", "badshell"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err == "error: unsupported shell 'badshell'; supported: bash, zsh, fish\n");

  // Parser-level refusal: parse_error -> 1 under THIS binary's policy
  // (the operator binary would say 2). Two streams, as the oracle does.
  auto const missing = dispatch(fx, {"completion"});
  CHECK(missing.code == 1);
  // Re-baselined onto CLI11's wording by task 6123 and pinned exactly. The
  // pair of exit codes out of ONE verb — 2 for the handler's refusal above,
  // 1 for the parser's here — is the part that cannot be satisfied by a
  // collapsed mapping, and it did not move.
  CHECK(missing.out == "error: shell is required\n");
  CHECK(missing.err == "error: RequiredError\n");
}

TEST_CASE("planar-watch schema appends the terminator its renderer omits", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("schema");
  auto const got = dispatch(fx, {"schema"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);

  REQUIRE(got.out.ends_with("\n"));
  CHECK(got.out.find('\n') == got.out.size() - 1);
  CHECK(got.out.contains("\"root\":\"planar-watch\""));
  CHECK(got.out.contains("\"planar-watch completion\""));
  // A catalog that named an unported verb would be worse than no catalog:
  // tools/cli_usage_lint.zig validates authored surfaces against it.
  CHECK_FALSE(got.out.contains("\"planar-watch feed\""));
}

TEST_CASE("planar-watch parse failures exit 1 and write both streams", "[cmd][watch][handlers][exitcode]") {
  auto const fx  = make_fixture("parse");
  auto const got = dispatch(fx, {"nosuchverb"});
  CHECK(got.code == 1);
  // Re-baselined onto CLI11's wording by task 6123 and pinned exactly.
  // The SHAPE is the contract and did not move: formatted message to
  // stdout, CamelCase tag to stderr, exit 1 (NOT the operator binary's 2).
  CHECK(got.out == "error: planar-watch: The following argument was not expected: nosuchverb\n");
  CHECK(got.err == "error: ExtrasError\n");
  CHECK_FALSE(got.db_open);
}

TEST_CASE("planar-watch help leads with the read-only prose block", "[cmd][watch][handlers]") {
  auto const fx  = make_fixture("help");
  auto const got = dispatch(fx, {"--help"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(got.db_open);

  // Task 6123 re-baseline. This case used to pin the `long_desc` branch of
  // the deleted `planar.cli.help` renderer, which emitted FLUSH LEFT where
  // the `desc` branch indented by two — planar-agent's root used the
  // latter, and getting the two backwards shifted every line of the page.
  // `CLI::App` carries ONE description string, so that distinction is gone
  // from the surface entirely (recorded in capability.t.cpp and in
  // `planar.cliapp.schema`'s header). What remains worth pinning is that
  // the OPERATOR-FACING CONTRACT STATEMENT still leads the page: the
  // sentence that tells a reader the handle is SQLITE_OPEN_READONLY and
  // that the verb set is the first line of defense. Losing it in
  // transcription would quietly delete the binary's own statement of what
  // it guarantees.
  CHECK(got.out.starts_with("planar-watch is the human-facing live cockpit for agent activity.\n"));
  CHECK(got.out.contains("(SQLITE_OPEN_READONLY) \xe2\x80\x94 every write SQL string is rejected by"));
  CHECK(got.out.contains("`no write verbs registered` capability boundary."));
  // The three ported verbs are listed; nothing else is.
  CHECK(got.out.contains("SUBCOMMANDS:"));
  CHECK(got.out.contains("version"));
  CHECK(got.out.contains("completion"));
  CHECK(got.out.contains("schema"));

  // A bare invocation renders the same page. DIVERGENCE, declared: the
  // oracle's default verb is `feed`, which is unported.
  auto const bare = dispatch(fx, {});
  CHECK(bare.code == 0);
  CHECK(bare.out == got.out);
}
