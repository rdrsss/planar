// @file dispatch.t.cpp
// @brief Tests for `planar.cmd.planar.dispatch` and the exit-code envelope
// (plan 996, tasks 6105 and 6123).
//
// HOME / DB SAFETY. Every context here is built over an explicit env map
// and a scratch database path, and every case in this file routes to a leaf
// that never opens SQLite (or fails before routing at all), so nothing here
// touches a real database.
//
// PROVENANCE, AND WHAT TASK 6123 RE-BASELINED.
//
// These parse-failure expectations used to be verbatim bytes from the Zig
// reference binary. Task 6123 deleted `src/lib/cli` — the hand-rolled
// parser that produced that exact wording — and made this binary drive
// CLI11 directly, so the WORDING is now CLI11's and is no longer
// oracle-comparable. The operator sanctioned that re-baseline explicitly.
//
// What did NOT change, and is still the reason these are pinned, is the
// SHAPE: one parse failure writes a FORMATTED message to STDOUT and a
// CamelCase error TAG to STDERR, and the operator binary exits 2 where
// planar-agent and planar-watch exit 1. Putting either message on the
// other stream, or collapsing the exit code, would look correct and be
// wrong. `CLI::ParseError::get_name()` already returns CamelCase
// (`ExtrasError`, `RequiredError`, `ValidationError`), so the stderr line
// needed no translation table.
//
// The bytes below were captured from the BUILT C++ binary the same way the
// oracle captures were taken — run under a scratch
// PLANAR_DB/PLANAR_HOME/PLANAR_CONFIG_PATH, read back through
// `python3 -c "print(repr(open(f,'rb').read()))"` — and are pinned exactly,
// not loosened to a `contains` check. A re-baselined expectation is only
// worth having if it still discriminates:
//
//   $C nosuchverb
//     exit 2
//     stdout b'error: planar: The following argument was not expected: nosuchverb\n'
//     stderr b'error: ExtrasError\n'
//
//   $C workflow nosuchsub
//     exit 2
//     stdout b'error: workflow: The following argument was not expected: nosuchsub\n'
//     stderr b'error: ExtrasError\n'
//
//   $C workflow list --nosuchflag
//     exit 2
//     stdout b'error: list: The following argument was not expected: --nosuchflag\n'
//     stderr b'error: ExtrasError\n'
//
//   $C workflow show          (missing required positional)
//     exit 2
//     stdout b'error: name is required\n'
//     stderr b'error: RequiredError\n'
//
// NOTE the error NAME is coarser than the oracle's: CLI11 reports an
// unknown subcommand, an unknown subcommand under a group, AND an unknown
// flag all as `ExtrasError`, where etcli distinguished
// `UnknownSubcommand` from `UnknownFlag`. The three cases stay separate
// tests because the stdout message still names the offending token and the
// command it was rejected under, which is the part an operator reads.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.surface;
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
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

} // namespace

TEST_CASE("path_key joins a resolved path with single spaces", "[cmd][dispatch]") {
  std::vector<std::string> const two{"workflow", "list"};
  std::vector<std::string> const one{"version"};
  std::vector<std::string> const none{};
  CHECK(planar::cliapp::path_key(two) == "workflow list");
  CHECK(planar::cliapp::path_key(one) == "version");
  CHECK(planar::cliapp::path_key(none).empty());
}

TEST_CASE("every leaf in the tree has a handler", "[cmd][dispatch][registration]") {
  // The registration gate. With ~200 verbs still to port, a tree node added
  // without a table entry is the single easiest mistake to make here, and
  // it is invisible until someone runs the verb and gets exit 64. This
  // turns it into a failing test at the moment the tree changes.
  auto const tree    = planar::cmd::root_app();
  auto const table   = planar::cmd::handlers(*tree);
  auto const missing = planar::cmd::unregistered_leaves(*tree, table);
  INFO("unwired leaves: " << std::format("{}", missing));
  CHECK(missing.empty());
}

TEST_CASE("every handler is reachable from the tree", "[cmd][dispatch][registration]") {
  // The other direction: a handler registered under a misspelled or removed
  // path is dead code that reads as coverage.
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::handlers(*tree);
  auto const dead  = planar::cmd::unreachable_handlers(*tree, table);
  INFO("unreachable handlers: " << std::format("{}", dead));
  CHECK(dead.empty());
}

TEST_CASE("unregistered_leaves actually reports an unwired leaf", "[cmd][dispatch][registration]") {
  // Break-probe in permanent form: the two gates above would pass just as
  // happily if `unregistered_leaves` always returned an empty vector. This
  // case proves it discriminates by handing it a table with a hole.
  auto const tree  = planar::cmd::root_app();
  auto       table = planar::cmd::handlers(*tree);
  table.erase("workflow show");
  auto const missing = planar::cmd::unregistered_leaves(*tree, table);
  REQUIRE(missing.size() == 1);
  CHECK(missing.front() == "workflow show");
}

TEST_CASE("unreachable_handlers actually reports a dead entry", "[cmd][dispatch][registration]") {
  auto const tree  = planar::cmd::root_app();
  auto       table = planar::cmd::handlers(*tree);
  table.emplace("workflow shwo", [](context&, const planar::cliapp::parsed_args&) -> planar::cmd::handler_result { return {}; });
  auto const dead = planar::cmd::unreachable_handlers(*tree, table);
  REQUIRE(dead.size() == 1);
  CHECK(dead.front() == "workflow shwo");
}

TEST_CASE("an unknown subcommand writes to BOTH streams and exits 2", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"nosuchverb"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: planar: The following argument was not expected: nosuchverb\n");
  CHECK(got.err == "error: ExtrasError\n");
}

TEST_CASE("an unknown subcommand under a group names the group", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"workflow", "nosuchsub"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: workflow: The following argument was not expected: nosuchsub\n");
  CHECK(got.err == "error: ExtrasError\n");
}

TEST_CASE("an unknown flag writes to BOTH streams and exits 2", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"workflow", "list", "--nosuchflag"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: list: The following argument was not expected: --nosuchflag\n");
  CHECK(got.err == "error: ExtrasError\n");
}

TEST_CASE("a missing required positional writes to BOTH streams and exits 2", "[cmd][dispatch][parity]") {
  auto const got = dispatch({"workflow", "show"});
  CHECK(got.code == 2);
  CHECK(got.out == "error: name is required\n");
  CHECK(got.err == "error: RequiredError\n");
}

TEST_CASE("--help renders the leaf's page to stdout and exits 0", "[cmd][dispatch]") {
  auto const got = dispatch({"workflow", "show", "--help"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  // Re-baselined onto CLI11's formatter (task 6123) and pinned exactly,
  // trailing spaces included — a page trimmed to a `contains` check would
  // stop discriminating a dropped flag, which is the whole reason a leaf's
  // help page is pinned at all.
  CHECK(got.out == "Show @meta and source path for a named workflow.\n"
                   "\n"
                   "\n"
                   "show [OPTIONS] name\n"
                   "\n"
                   "\n"
                   "POSITIONALS:\n"
                   "  name REQUIRED               \n"
                   "\n"
                   "OPTIONS:\n"
                   "  -h,     --help              Print this help message and exit\n"
                   "          --json              \n");
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
  auto const         tree = planar::cmd::root_app();
  int const          code = planar::cmd::run(ctx, *tree, planar::cmd::handler_table{});
  CHECK(code == 64);
  CHECK(err.str() == "error: not implemented yet\n");
  CHECK(out.str().empty());
}

TEST_CASE("a declared-but-unported LEAF refuses at exit 64, naming itself", "[cmd][dispatch][not-implemented]") {
  // THE HEADLINE PROPERTY of the full-surface declaration (plan 996, task
  // 6065). `planar` declares all 223 of the oracle's leaves so that
  // `zig/tools/cli_usage_lint` can resolve every authored command path;
  // 188 of them land no behaviour. A declared node that dispatches to
  // nothing is WORSE than an absent one if it exits 0, so each refuses
  // loudly and names itself.
  auto const leaf = dispatch({"plan", "list"});
  CHECK(leaf.code == 64);
  CHECK(leaf.out.empty());
  CHECK(leaf.err == "error: plan list: not implemented in this build\n");

  // Deeper, to prove the key is the full path and not the leaf name.
  auto const deep = dispatch({"feedback", "triage", "list"});
  CHECK(deep.code == 64);
  CHECK(deep.err == "error: feedback triage list: not implemented in this build\n");

  // Discrimination: a PORTED verb on the same binary does not answer 64,
  // so exit 64 is not simply what this binary now does.
  auto const ported = dispatch({"version"});
  CHECK(ported.code == 0);
  CHECK(ported.err.empty());
}

TEST_CASE("a declared-but-unported DUAL node refuses too, instead of exiting 0", "[cmd][dispatch][not-implemented]") {
  // The hazard that makes the explicit registration load-bearing rather
  // than decorative. `run` renders a matched node's HELP PAGE and returns
  // exit_success whenever the node has children and the table has no entry
  // for it. So a node that is a group AND a verb in the oracle — measured
  // by invoking all 38 of the oracle's group nodes against a scratch
  // arena, and there are exactly three: `resume`, `handoff`, `health` —
  // would have exited 0 with a help page where the oracle does real work.
  // A SILENT SUCCESS, which is the one outcome a declared-but-unported
  // verb must never produce.
  //
  // `health` is the one this task declared. Oracle-captured against a
  // pinned scratch arena: `planar health` exits 0 having printed a
  // contributor report — it is a verb, not a group heading.
  auto const dual = dispatch({"health"});
  CHECK(dual.code == 64);
  CHECK(dual.out.empty());
  CHECK(dual.err == "error: health: not implemented in this build\n");

  // ...and the group half of the same node still resolves its children.
  auto const child = dispatch({"health", "hygiene"});
  CHECK(child.code == 64);
  CHECK(child.err == "error: health hygiene: not implemented in this build\n");

  // The discrimination: a PURE group with no handler must still render
  // help at exit 0, because that is what the oracle does for the other 35.
  auto const pure = dispatch({"plan"});
  CHECK(pure.code == 0);
  CHECK(pure.err.empty());
  CHECK(pure.out.contains("next"));

  // And the break-probe for THIS case: if `health` were merely declared
  // and left out of the table, it would take the `pure` path above. The
  // table entry is what separates them, so removing it must flip the
  // behaviour.
  auto const tree  = planar::cmd::root_app();
  auto       table = planar::cmd::handlers(*tree);
  REQUIRE(table.erase("health") == 1);
  std::ostringstream out;
  std::ostringstream err;
  auto const         root = std::filesystem::temp_directory_path() / "planar_dispatch_health_probe";
  std::error_code    ec;
  std::filesystem::create_directories(root, ec);
  context   ctx{{"planar", "health"}, planar::cmd::map_env({}), root, root / "planar.db", out, err};
  int const code = planar::cmd::run(ctx, *tree, table);
  CHECK(code == 0);
  CHECK(out.str().contains("hygiene"));
  CHECK(err.str().empty());
}

TEST_CASE("every leaf is in exactly one of the two handler populations", "[cmd][dispatch][registration]") {
  // The gate that keeps the generated `unported_paths()` inventory honest
  // as verbs get ported. Two failure modes, both silent without this:
  // an inventory entry that shadows a verb someone just implemented (the
  // bulk `emplace` is a no-op on an existing key, so the real handler
  // wins, but the stale entry would then be invisible), and a leaf in
  // neither population (caught by `unregistered_leaves`, above).
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::handlers(*tree);

  std::set<std::string, std::less<>> unported;
  for (auto const& verb : planar::cmd::unported_paths()) {
    unported.emplace(verb);
  }
  CHECK(unported.size() == 188);
  // The three duals are the entries that are NOT leaves; `resume` and
  // `handoff` have real handlers, so `health` is the only one here.
  CHECK(unported.contains("health"));
  CHECK_FALSE(unported.contains("resume"));
  CHECK_FALSE(unported.contains("handoff"));
  // No implemented verb may appear in the inventory.
  for (auto const& implemented : {"version", "schema", "completion", "unlink", "workbench gc", "annotate add", "capture snapshot",
                                  "handoff show", "resume validate", "ext list"}) {
    INFO("implemented verb wrongly listed as unported: " << implemented);
    CHECK_FALSE(unported.contains(implemented));
  }

  auto const leaves = planar::cliapp::leaf_keys(*tree);
  CHECK(leaves.size() == 223);
  for (auto const& leaf : leaves) {
    INFO("leaf: " << leaf);
    CHECK(table.contains(leaf));
  }
}

TEST_CASE("the exit-code envelope maps each bucket distinctly", "[cmd][exit]") {
  // Exit-code mapping is the easiest thing in this task to test vacuously:
  // a table collapsed to a single value passes any test that only checks
  // one bucket. These five are checked together, and the four the binary
  // actually produces (0, 1, 2, 64) are each also produced end to end
  // elsewhere in this file and in handlers.t.cpp.
  using planar::cmd::domain_error_kind;
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::generic_failure, "x")) == 1);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::not_found, "x")) == 1);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::invalid_input, "x")) == 2);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::parse_error, "x")) == 2);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::sync_conflict, "x")) == 3);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::scope_mismatch, "x")) == 5);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::slug_conflict, "x")) == 6);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::schema_version_ahead, "x")) == 7);
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::not_implemented, "x")) == 64);

  // The `planar` binary's own divergence, not the agent binary's: this
  // module owns a COMPLETE, local table (task 6123 removed the shared
  // binary-parameterized one), so schema_version_behind is 1 here — no arm
  // in zig/src/cmd/planar/exit.zig's codeFor — where it is 7 on
  // planar-agent / planar-watch.
  CHECK(planar::cmd::exit_code(planar::cmd::error_from_body(planar::cmd::domain_error_kind::schema_version_behind, "x")) == 1);
}

TEST_CASE("report composes a message body but writes a rendered payload verbatim", "[cmd][exit]") {
  // The two stderr shapes. Getting this wrong doubles the `error: ` prefix
  // on every renderer-sourced failure, which is precisely what `workflow
  // show nope` would have shown.
  using planar::cmd::domain_error_kind;
  std::ostringstream body_out;
  planar::cmd::report(planar::cmd::error_from_body(planar::cmd::domain_error_kind::invalid_input, "--anchor-path is required"),
                      body_out);
  CHECK(body_out.str() == "error: --anchor-path is required\n");

  std::ostringstream rendered_out;
  planar::cmd::report(
      planar::cmd::error_from_rendered(planar::cmd::domain_error_kind::generic_failure, "error: workflow 'nope' not found\n"),
      rendered_out);
  CHECK(rendered_out.str() == "error: workflow 'nope' not found\n");
}

// --- dual group-and-leaf nodes (plan 996, task 6040) -------------------
//
// `handoff` and `resume` each carry subcommands AND their own handler.
// Before this task, dispatch rendered a help page for ANY matched node
// with children, which would have made `planar handoff 2` exit 0 with a
// help page where the oracle runs the ritual. Oracle-captured:
//
//   $Z handoff   -> exit 2, error: no active session (run `planar capture
//                   session` first)
//   $Z resume    -> exit 1, error: no active task in cwd-derived scope;
//                   pass <task-id> explicitly
//   $Z capture   -> exit 0, help page      (a PURE group)
//
// The narrowed rule is "help only when the table has no entry for the
// node". These cases pin both sides of it.

TEST_CASE("a dual group-and-leaf node dispatches to its own handler", "[cmd][dispatch][registration]") {
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::handlers(*tree);

  // Both parents are registered even though `cliapp::leaf_keys` — which
  // only counts CHILDLESS nodes — does not list them.
  CHECK(table.contains("handoff"));
  CHECK(table.contains("resume"));

  auto const leaves = planar::cliapp::leaf_keys(*tree);
  CHECK(std::ranges::find(leaves, "handoff") == leaves.end());
  CHECK(std::ranges::find(leaves, "resume") == leaves.end());

  // ...and `unreachable_handlers` must NOT call them dead. It walks every
  // node, not just leaves, precisely so this holds.
  auto const dead = planar::cmd::unreachable_handlers(*tree, table);
  CHECK(std::ranges::find(dead, "handoff") == dead.end());
  CHECK(std::ranges::find(dead, "resume") == dead.end());
}

TEST_CASE("a PURE group with no handler still renders help", "[cmd][dispatch]") {
  // The other side of the narrowed rule. `capture` has six children and no
  // handler of its own, so it must keep the help-page behaviour.
  auto const tree  = planar::cmd::root_app();
  auto const table = planar::cmd::handlers(*tree);
  REQUIRE_FALSE(table.contains("capture"));

  auto const got = dispatch({"capture"});
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out.contains("session"));
  CHECK(got.out.contains("snapshot"));
}

TEST_CASE("unreachable_handlers still reports a key naming no node at all", "[cmd][dispatch][registration]") {
  // Widening the walk from leaves to every node must NOT have blunted the
  // gate: a key that names nothing is still dead. Without this case the
  // widening could have been "return {} always" and every gate above would
  // pass.
  auto const tree  = planar::cmd::root_app();
  auto       table = planar::cmd::handlers(*tree);
  table.emplace("handoff nosuchchild",
                [](context&, const planar::cliapp::parsed_args&) -> planar::cmd::handler_result { return {}; });
  auto const dead = planar::cmd::unreachable_handlers(*tree, table);
  REQUIRE(dead.size() == 1);
  CHECK(dead.front() == "handoff nosuchchild");
}
