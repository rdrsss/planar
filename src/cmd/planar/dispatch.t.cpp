// @file dispatch.t.cpp
// @brief Tests for `planar.cmd.planar.dispatch` and the exit-code envelope
// (plan 996, task 6105).
//
// HOME / DB SAFETY. Every context here is built over an explicit env map
// and a scratch database path, and every case in this file routes to a leaf
// that never opens SQLite (or fails before routing at all), so nothing here
// touches a real database.
//
// ORACLE PROVENANCE. The three parse-failure captures below are verbatim
// bytes from the reference binary run under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH, read back through
// `python3 -c "print(repr(open(f,'rb').read()))"`:
//
//   $Z nosuchverb
//     exit 2
//     stdout b'error: unknown subcommand (got nosuchverb) [in: planar]\n'
//     stderr b'error: UnknownSubcommand\n'
//
//   $Z workflow nosuchsub
//     exit 2
//     stdout b'error: unknown subcommand (got nosuchsub) [in: workflow]\n'
//     stderr b'error: UnknownSubcommand\n'
//
//   $Z workflow list --nosuchflag
//     exit 2
//     stdout b'error: unknown flag (got --nosuchflag)\n'
//     stderr b'error: UnknownFlag\n'
//
//   $Z workflow show          (missing required positional)
//     exit 2
//     stdout b'error: required positional missing: <name>\n'
//     stderr b'error: MissingRequiredPositional\n'
//
// The dual-stream shape is the surprising part and the reason these are
// pinned: a parse failure writes a FORMATTED message to STDOUT (etcli's own
// formatter, writing to the writer `cli.dispatch` was handed) and a
// CamelCase error TAG to STDERR (main.zig's `exit.die`). Putting either on
// the other stream would look correct and be wrong.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cli;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

/// @brief What one `dispatch::run` invocation produced.
struct invocation {
  int         code = 0; ///< The exit code returned by `run`.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief Run the real tree and the real handler table over `args`.
/// @param args The argv tail (argv[0] is supplied).
/// @param vars The environment the context exposes.
/// @return The captured invocation.
auto dispatch(std::vector<std::string> args, std::map<std::string, std::string, std::less<>> vars = {}) -> invocation {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_dispatch_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(std::move(vars)), root, root / "planar.db", out, err};
  auto const         tree  = planar::cmd::root_command();
  auto const         table = planar::cmd::handlers();
  int const          code  = planar::cmd::run(ctx, tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

} // namespace

TEST_CASE("path_key joins a resolved path with single spaces", "[cmd][dispatch]") {
  std::vector<std::string> const two{"workflow", "list"};
  std::vector<std::string> const one{"version"};
  std::vector<std::string> const none{};
  CHECK(planar::cmd::path_key(two) == "workflow list");
  CHECK(planar::cmd::path_key(one) == "version");
  CHECK(planar::cmd::path_key(none).empty());
}

TEST_CASE("every leaf in the tree has a handler", "[cmd][dispatch][registration]") {
  // The registration gate. With ~200 verbs still to port, a tree node added
  // without a table entry is the single easiest mistake to make here, and
  // it is invisible until someone runs the verb and gets exit 64. This
  // turns it into a failing test at the moment the tree changes.
  auto const tree    = planar::cmd::root_command();
  auto const table   = planar::cmd::handlers();
  auto const missing = planar::cmd::unregistered_leaves(tree, table);
  INFO("unwired leaves: " << std::format("{}", missing));
  CHECK(missing.empty());
}

TEST_CASE("every handler is reachable from the tree", "[cmd][dispatch][registration]") {
  // The other direction: a handler registered under a misspelled or removed
  // path is dead code that reads as coverage.
  auto const tree  = planar::cmd::root_command();
  auto const table = planar::cmd::handlers();
  auto const dead  = planar::cmd::unreachable_handlers(tree, table);
  INFO("unreachable handlers: " << std::format("{}", dead));
  CHECK(dead.empty());
}

TEST_CASE("unregistered_leaves actually reports an unwired leaf", "[cmd][dispatch][registration]") {
  // Break-probe in permanent form: the two gates above would pass just as
  // happily if `unregistered_leaves` always returned an empty vector. This
  // case proves it discriminates by handing it a table with a hole.
  auto const tree  = planar::cmd::root_command();
  auto       table = planar::cmd::handlers();
  table.erase("workflow show");
  auto const missing = planar::cmd::unregistered_leaves(tree, table);
  REQUIRE(missing.size() == 1);
  CHECK(missing.front() == "workflow show");
}

TEST_CASE("unreachable_handlers actually reports a dead entry", "[cmd][dispatch][registration]") {
  auto const tree  = planar::cmd::root_command();
  auto       table = planar::cmd::handlers();
  table.emplace("workflow shwo", [](context&, const planar::cli::match_result&) -> planar::cmd::handler_result { return {}; });
  auto const dead = planar::cmd::unreachable_handlers(tree, table);
  REQUIRE(dead.size() == 1);
  CHECK(dead.front() == "workflow shwo");
}

TEST_CASE("an unknown subcommand writes to BOTH streams and exits 2", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"nosuchverb"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: unknown subcommand (got nosuchverb) [in: planar]\n");
  CHECK(got.err == "error: UnknownSubcommand\n");
}

TEST_CASE("an unknown subcommand under a group names the group", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"workflow", "nosuchsub"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: unknown subcommand (got nosuchsub) [in: workflow]\n");
  CHECK(got.err == "error: UnknownSubcommand\n");
}

TEST_CASE("an unknown flag writes to BOTH streams and exits 2", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"workflow", "list", "--nosuchflag"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: unknown flag (got --nosuchflag)\n");
  CHECK(got.err == "error: UnknownFlag\n");
}

TEST_CASE("a missing required positional writes to BOTH streams and exits 2", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"workflow", "show"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: required positional missing: <name>\n");
  CHECK(got.err == "error: MissingRequiredPositional\n");
}

TEST_CASE("--help renders the leaf's page to stdout and exits 0", "[cmd][dispatch]") {
  auto const got = dispatch({"workflow", "show", "--help"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == "workflow show\n"
                   "\n"
                   "  Show @meta and source path for a named workflow.\n"
                   "\n"
                   "USAGE:\n"
                   "  workflow show [flags] <name>\n"
                   "\n"
                   "FLAGS:\n"
                   "  --json                (bool) default=false\n"
                   "\n"
                   "POSITIONAL ARGUMENTS:\n"
                   "  <name>          (string)\n");
}

TEST_CASE("an unwired leaf falls through to exit 64, not a crash", "[cmd][dispatch]") {
  // The routing fallthrough the registration gate exists to make
  // unreachable. Exercised here with a deliberately emptied table so the
  // path is covered even though no real leaf can reach it.
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_dispatch_64_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{{"planar", "version"}, planar::cmd::map_env({}), root, root / "planar.db", out, err};
  auto const         tree = planar::cmd::root_command();
  int const          code = planar::cmd::run(ctx, tree, planar::cmd::handler_table{});
  CHECK(code == 64);
  CHECK(err.str() == "error: not implemented yet\n");
  CHECK(out.str().empty());
}

TEST_CASE("the exit-code envelope maps each bucket distinctly", "[cmd][exit]") {
  // Exit-code mapping is the easiest thing in this task to test vacuously:
  // a table collapsed to a single value passes any test that only checks
  // one bucket. These five are checked together, and the four the binary
  // actually produces (0, 1, 2, 64) are each also produced end to end
  // elsewhere in this file and in handlers.t.cpp.
  using planar::cli::domain_error_kind;
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::generic_failure, "x")) == 1);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::not_found, "x")) == 1);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::invalid_input, "x")) == 2);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::parse_error, "x")) == 2);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::sync_conflict, "x")) == 3);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::scope_mismatch, "x")) == 5);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::slug_conflict, "x")) == 6);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::schema_version_ahead, "x")) == 7);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::not_implemented, "x")) == 64);

  // The `planar` binary's own divergence, not the agent binary's: this
  // module hard-wires binary_kind::planar, so schema_version_behind is 1
  // here (no arm in zig/src/cmd/planar/exit.zig's codeFor) where it is 7 on
  // planar-agent / planar-watch.
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(domain_error_kind::schema_version_behind, "x")) == 1);
}

TEST_CASE("report composes a message body but writes a rendered payload verbatim", "[cmd][exit]") {
  // The two stderr shapes. Getting this wrong doubles the `error: ` prefix
  // on every renderer-sourced failure, which is precisely what `workflow
  // show nope` would have shown.
  using planar::cli::domain_error_kind;
  std::ostringstream body_out;
  planar::cmd::report(planar::cmd::error_from_body(domain_error_kind::invalid_input, "--anchor-path is required"), body_out);
  CHECK(body_out.str() == "error: --anchor-path is required\n");

  std::ostringstream rendered_out;
  planar::cmd::report(planar::cmd::error_from_rendered(domain_error_kind::generic_failure, "error: workflow 'nope' not found\n"),
                      rendered_out);
  CHECK(rendered_out.str() == "error: workflow 'nope' not found\n");
}
