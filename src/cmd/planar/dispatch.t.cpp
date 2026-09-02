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
  // 92 of them land no behaviour (task 6196). A declared node that dispatches to
  // nothing is WORSE than an absent one if it exits 0, so each refuses
  // loudly and names itself.
  //
  // The exemplar was `plan list` until task 6141 ported it, then `question
  // list` until task 6188 ported that one too. It has to be a leaf with no
  // REQUIRED positional, or the parser refuses at exit 2 before dispatch is
  // reached at all and the case would assert the parser's behaviour rather
  // than the table's — which is what `plan diff` did on the first attempt
  // at this replacement. It was `scenario list` until task 6195 ported
  // that one too, and `artifact list` until 6196 ported the last planning
  // family out from under it.
  //
  // With every planning family's CRUD half now ported, the exemplar has to
  // leave the planning surface entirely. It was `assoc list` until task
  // 6279 ported that too — and the note the previous cycle left here ("the
  // next cycle that ports `assoc list` must reach for a different family
  // again") is exactly what happened, one cycle later.
  //
  // It was `dashboard` until task 6329 ported it — and the note that stood
  // here called it "blocked on the agent-claim roll-up its `--agents` arm
  // needs", which was the same stale reading as `surface.cpp`'s: that
  // roll-up (`agentactivity`'s claim and next-work readers, `agentrender`'s
  // fragment writers) had been in the tree for milestones.
  //
  // It was `report` until task 6352 ported it, once `engine_introspect`
  // and `engine_introspection_adapters` were both complete and decision
  // 981's layer-1 extraction let them meet through `bundle::preview`
  // without an `engine_* -> engine_*` edge — see handlers/report.cppm.
  //
  // It is now `explore`, the most blocked leaf in the inventory: the
  // handler is 93 lines, which is why it reads cheap, but it launches the
  // COCKPIT, 33,452 zig lines under `cmd/planar/cockpit/` with no C++
  // counterpart at all (see the `unported_paths` inventory test below for
  // the full account). TOP-LEVEL rather than a family member (no sibling
  // port can drag it along by accident) and arg-free — no positionals at
  // all in `surface.cpp`, so the parser cannot refuse at exit 2 before
  // dispatch is reached.
  auto const leaf = dispatch({"explore"});
  CHECK(leaf.code == 64);
  CHECK(leaf.out.empty());
  CHECK(leaf.err == "error: explore: not implemented in this build\n");

  // And the sibling that left the inventory at task 6352 answers its own
  // verb instead of 64, which is what makes the row above a statement
  // about `explore` rather than about top-level leaves in general.
  CHECK(dispatch({"report", "--help"}).code == 0);

  // Deeper, to prove the key is the full path and not the leaf name.
  //
  // This was `feedback triage list` until task 6303 ported that family, then
  // `workspace routing build` until task 6275 ported that one. The exemplar
  // has two constraints: it must be genuinely unported, and its positionals
  // must all be OPTIONAL or the parser refuses at exit 2 before dispatch is
  // reached.
  //
  // With `workspace routing build` gone there is NO three-level path left
  // that satisfies both. The inventory's only other three-level entry was
  // `task touches infer`, whose `task-id` is required — so it would have
  // failed at the parser and asserted the parser's behaviour rather than the
  // table's. The replacement is therefore two-level rather than three, and
  // the property under test is unaffected: `init` is a leaf name that exists
  // only under `workspace`, so answering to the full path `workspace init`
  // still proves the key is the path.
  //
  // Worth stating plainly so the next cycle does not go hunting: three
  // levels is not recoverable here. Task 6330 ported `task touches infer`,
  // so the inventory now holds NO three-level path at all — it comes back
  // only if some future cycle declares a new three-level family.
  //
  // `workspace init` is a handler now, so this optional-only leaf reaches
  // its domain guard rather than the generated exit-64 placeholder.
  auto const deep = dispatch({"workspace", "init"});
  CHECK(deep.code != 64);

  // Discrimination: a PORTED verb on the same binary does not answer 64,
  // so exit 64 is not simply what this binary now does.
  auto const ported = dispatch({"version"});
  CHECK(ported.code == 0);
  CHECK(ported.err.empty());
}

TEST_CASE("the health DUAL node's parent and child are BOTH wired, and both matter to the same hazard",
          "[cmd][dispatch][registration]") {
  // The hazard the explicit table registration exists to close: `run`
  // renders a matched node's HELP PAGE and returns exit_success whenever
  // the node has children and the table has no entry for it. So a node
  // that is a group AND a verb in the oracle — measured by invoking all 38
  // of the oracle's group nodes against a scratch arena, and there are
  // exactly three: `resume`, `handoff`, `health` — would silently exit 0
  // with a help page where the oracle does real work, if its table entry
  // were ever dropped.
  //
  // `health` used to be the one member of that trio still declared-but-
  // unported (exit 64), until task 6357 closed the family. Both halves of
  // the dual node are now genuinely wired, so this case asserts the
  // POSITIVE property directly rather than the refusal it used to pin.
  auto const scratch_home = std::filesystem::temp_directory_path() / "planar_dispatch_health_probe_home";
  auto const parent       = dispatch({"health"}, {{"HOME", scratch_home.string()}});
  CHECK(parent.code == 0);
  CHECK(parent.err.empty());
  CHECK(parent.out.contains("overall:          ok"));

  // The child, `health hygiene`, was already ported at task 6090 — this
  // reconfirms the parent's real work does NOT swallow it.
  auto const child = dispatch({"health", "hygiene"});
  CHECK(child.code == 0);
  CHECK(child.err.empty());
  CHECK(child.out.contains("=== Stale draft plans"));

  // The discrimination: a PURE group with no handler must still render
  // help at exit 0, because that is what the oracle does for the other 35.
  auto const pure = dispatch({"plan"});
  CHECK(pure.code == 0);
  CHECK(pure.err.empty());
  CHECK(pure.out.contains("next"));

  // And the break-probe for the hazard itself: with `health`'s table entry
  // removed, the SAME invocation must fall back to the `pure` group path
  // above (a help page at exit 0) rather than doing real work — proving
  // the table entry, not some other mechanism, is what selects the
  // handler.
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
  // 188 before task 6132 ported `init` out of the inventory; 187 before
  // task 6133 ported `plan create` and `assoc create` out of it; 185
  // before task 6135 ported `task add` and `assoc add` out of it; 183
  // before task 6141 ported ELEVEN more out of it — the four `plan` leaves
  // with engine support (`show`, `list`, `update`, `recompute-status`) and
  // the seven `task` ones (`show`, `list`, `update`, `done`, `cancel`,
  // `block`, `reopen`); 172 before task 6148 ported the remaining TWELVE
  // `annotate` leaves out of it (`show`, `update`, `remove`, `tag`,
  // `resolve`, `dismiss`, `archive`, the three `bulk-*`, `verify` and
  // `sweep`), completing the family that `add` and `list` opened; 160
  // before task 6149 ported TWENTY-TWO more out of it — the thirteen
  // `models` leaves with engine support (`evals`, `experiments`,
  // `outcomes` and all ten under `registry`), the five ported `bench`
  // leaves (`start`, `event`, `touch`, `finish`, `show`) and all four
  // `run` leaves (`start`, `event`, `finish`, `show`); 138 before task 6187
  // ported EIGHT more out of it — the whole five-leaf `plan step` family
  // (`add`, `list`, `done`, `skip`, `link`), landed together with the
  // `plan_steps` engine that had no port at all, and three of the four
  // `task touches` leaves (`add`, `list`, `remove`).
  //
  // Three leaves from those families STAYED in the inventory on purpose,
  // and the distinction is the reason this count is 130 rather than 127:
  // `bench harvest`, `models resolve` and `task touches infer` are each
  // blocked at LAYER 2, with their engine halves deferred alongside their
  // dependencies (a git-subprocess seam, the 2,725-line roles/profile/
  // packet subsystem, and `planning/touchinfer.zig`'s 773 lines of
  // git-diff-and-language-aware path inference respectively).
  //
  // CORRECTION (task 6330): that last characterisation was wrong, and the
  // cycle that ported the leaf established it by RUNNING the oracle.
  // `touchinfer.zig` shells nothing, imports no git and knows no languages;
  // it is whitespace tokenisation plus `stat()`. It was never blocked, only
  // unported. The sentence is left standing as the historical record of why
  // the count was 130 at the time, with this note beside it. Wiring a
  // handler over an absent engine would mean inventing behaviour;
  // refusing at exit 64 by name does not.
  //
  // 130 before task 6189 ported SEVEN more out of it — the whole five-leaf
  // `local` family (`list`, `link`, `unlink`, `import`, `migrate`), plus
  // `closure show` and `groups recommend`, the read halves of two
  // single-leaf families. `closure compute` STAYED, for the same
  // layer-2 reason as the three above it: its extractor is 1637 lines of
  // tree-sitter AST work over a filesystem corpus walk, and tree-sitter is
  // not vendored in this tree. That leaves `closure` a family with one
  // ported leaf and one declared refusal, which is the shape `bench` and
  // `models` already have.
  //
  // This number is load-bearing: it is what fails when a verb gains a
  // handler and its generated inventory entry is not dropped in the same
  // change. Update it WITH the port; never widen the check to make it stop
  // firing.
  //
  // 123 before task 6188 ported SIX more out of it — `question add`,
  // `show`, `list`, `answer` and `wontfix` (the `question` family's CRUD
  // and transition half), plus `assoc members`, whose engine call already
  // existed and whose only missing piece was a `project_ref` list
  // renderer. The `question` family's other FIVE stayed, and their reasons
  // are asserted individually below: `edit`/`view`/`diff`/`review` are the
  // workbench drafting quartet (unported editflow plumbing), and `question
  // link` is one arm of the shared entity-link surface whose other six
  // arms all still refuse.
  //
  // 117 before task 6193 ported the last SEVEN out of it — the whole
  // entity-link surface: `links add`, `links list`, `links remove`,
  // `links trail`, `plan link`, `task link` and `question link`.
  // `engine_entitylink` had been fully ported since task cpp-entity-links
  // and exported no RENDERER, which was the single reason all seven
  // refused; task 6188 deliberately left `question link` behind rather
  // than ship one working arm beside six refusing ones. They went in
  // together, and the `question` family's remaining refusals dropped from
  // five to four.
  //
  // 110 before task 6194 ported SEVEN more out of it — the `decision`
  // family's CRUD, transition and link half: `decision add`, `show`,
  // `list`, `accept`, `supersede`, `withdraw` and `link`. `decision link`
  // went in WITH them rather than being held back the way `question link`
  // was, because the shared entity-link surface it forwards into had
  // already landed at 6193; holding it would have left the incoherence the
  // earlier hold existed to avoid, only mirrored. The family's other FOUR
  // stay, named individually below.
  //
  // 103 before task 6195 ported SIX more out of it — the `scenario`
  // family's CRUD, transition and link half: `scenario add`, `show`,
  // `list`, `verify`, `retire` and `link`. `scenario link` went in WITH
  // them, on the reason 6194 established: the shared entity-link surface
  // landed at 6193, so holding one arm back now creates the incoherence
  // the original hold existed to avoid. The family's other FOUR stay,
  // named individually below.
  //
  // Two things about this port are worth reading before `artifact`, the
  // last unported planning family, lands:
  //   - Its EMPTY `--status` filter means EVERY status, `retired`
  //     included, where `question`'s means `open` and `decision`'s means
  //     `{proposed, accepted}`. None of the three is derivable from
  //     another; all three were captured by running them.
  //   - `scenario list --touches` is SERVED rather than refused at exit 64
  //     the way `plan list --touches` and `task list --touches` are. Its
  //     `listTouching` half is ported, so `touches_not_implemented` has no
  //     call site in this family.
  //
  // 97 before task 6196 ported FIVE more out of it — the `artifact`
  // family's CRUD and link half: `artifact add`, `show`, `list`, `update`
  // and `link`. `artifact link` went in WITH them, same reason as 6194's
  // and 6195's. That leaves 92, and it completes the planning ENGINE
  // surface: every planning entity's CRUD half is now ported.
  //
  // The family is NINE leaves, not five, and the four that stayed are the
  // workbench drafting quartet a FOURTH time. Their blocker has NARROWED
  // and the new statement of it matters for whoever picks it up:
  // `engine_workbench` IS ported and all ten `workbench` leaves are wired,
  // so the remaining dependency is `zig/src/cmd/planar/editflow.zig` (2076
  // lines) plus `editor.zig` (342) — CMD-layer, not engine. One module
  // gates SIXTEEN leaves across all four families, which is why it wants
  // its own task rather than riding in on a per-family engine port.
  //
  // Three things about `artifact` are worth reading before that task:
  //   - Its EMPTY `--status` filter means `{draft, active}` — the two
  //     terminal statuses are off-screen. That is a FOURTH distinct answer
  //     from four sibling families (`question` = `open`, `decision` =
  //     `{proposed, accepted}`, `scenario` = every status). None is
  //     derivable from another; all four were captured by running them.
  //   - `artifact add` STARTS A SESSION, and it is the only leaf in the
  //     family that touches `sessions`. `scenario add` — its closest
  //     sibling — starts none. The ordering is observable: `--kind nosuch`
  //     and `--body @missing` refuse with sessions = 0, `--scope nosuch`
  //     refuses with sessions = 1.
  //   - `artifact add --plan 999` REFUSES at exit 1 and writes nothing,
  //     where `scenario add --plan 4242` succeeds and leaves a dangling
  //     edge. One flag name, opposite answers, both captured.
  //
  // 62 and 65 respectively when tasks 6208 and 6214 each measured this
  // against `68a1cba`; they landed together, so the count is 57. Each batch
  // is asserted PER LEAF below — the arithmetic here agreeing is not what
  // makes either correct.
  //
  // 70 before task 6208 ported the last EIGHT drafting leaves out of it —
  // `plan` and `task`'s `edit | view | diff | review`. 6205 had held them
  // deliberately (the block further down was the guard); 6208 is the oracle
  // run it held them FOR, and the run earned its cycle: `plan` and `task`
  // report `not_found` where the link-anchored four report `no_plan_link`,
  // so their `diff`/`review` failure prose differs. That is asserted per
  // leaf below rather than trusted to this count. With them the drafting
  // quartet is complete across all six planning families — twenty-four
  // leaves over one shared cmd-layer module.
  //
  // 70 before task 6214 ported the whole FIVE-leaf `scope` family out of it:
  // `show`, `suggest`, `use`, `pop`, `clear`. The last three are the
  // plan-153-M5 removal refusals — they leave this inventory because a
  // refusal the oracle OWNS is a port, where the exit-64 default is a
  // placeholder claiming the verb might one day work. Named per leaf below.
  //
  // 57 before task 6090 ported THREE more out of it: `search`,
  // `health hygiene` and `audit session`. That leaves 54.
  //
  // Task 6090's scope was NINE leaves (five `audit`, two `health`, one
  // `tree`, one `search`) and it deliberately took three completely rather
  // than nine partially. What the run established about the other six is
  // worth reading before anyone picks them up, because four of them are
  // BLOCKED on a dependency rather than merely large:
  //   - `health` needs `engine.installedsurface` (548 unported Zig lines of
  //     manifest-driven filesystem classification). Its `check` half ports
  //     easily; the leaf does not, because `projection_freshness` feeds the
  //     `overall` rollup the verb's exit-1-on-degraded contract reads.
  //   - `audit commits` needs `engine.runtime.sessioncommits` (1205 lines
  //     of git subprocess walks) — the same blocker `capture commits` and
  //     `bench harvest` already carry.
  //   - `audit publish-decision` needs an adapter INSTANCE, i.e. the
  //     auth-resolving adapter factory `ext create`/`propagate`/`test` and
  //     the three `sync` leaves are also waiting on.
  //   - `tree` and `audit trail` are merely large (1284 and 563 non-test
  //     Zig lines). `audit trail`'s ENTITY arm is ALREADY SERVED by the
  //     `planar.engine.runtime.audit_trail` module this task landed; what
  //     it still lacks is the `external_links` / `sync_events` read path
  //     for its link-id arm.
  //
  // TASK 6262 CORRECTS THE LAST BULLET AND MOVES ONE LEAF. `audit trail`
  // is ported; 53 -> 52. Its link-id arm did NOT need a new read path —
  // `engine_external`'s `link::show`, `system::show_by_id` and
  // `sync::events_for_link` were already in the tree by the time the leaf
  // was picked up, so the "no module has it" note above was true when
  // written and stale when acted on. The commits fold-in did need
  // something new, and got the PURE-SQL read half of `sessioncommits`
  // (`list_for_sessions`); that does not touch `audit commits`, whose
  // blocker is the git-walk half.
  //
  // `tree` was in this cycle's scope too and is deliberately UNTOUCHED
  // rather than half-ported. It stays in the inventory below.
  //
  // The task body's `listTouching` acceptance criterion was checked and is
  // STALE, not skipped: `list_plans_touching` / `list_tasks_touching` /
  // `list_questions_touching` / `list_scenarios_touching` all exist and all
  // four `--touches` flags are wired. Task 6187 landed them.
  // 57 before task 6258 ported ONE leaf out of it — `ext test` — together
  // with the adapter FACTORY it had been deferred on since task 6041. One
  // leaf for a whole cycle is the honest count: the task predicted eight
  // (the `sync` trio and the three remaining `ext` leaves alongside it),
  // and that estimate did not survive the factory existing. Those six are
  // blocked on the create/propagate half of `engine_extsync` and on there
  // being no `sync` handler at this layer — absences the factory does not
  // touch. See src/cmd/planar/CMakeLists.txt's task-6258 section.
  //
  // 53 before task 6259 ported the whole FIVE-leaf `config` family out of
  // it: `show`, `edit`, `validate`, `init`, `path`. Named per leaf below
  // rather than trusted to the count, same reason as every batch above it.
  //
  // Nothing was blocking these beyond the wiring itself: the engine half
  // (`planar.engine.config`) landed complete in commit 82820b7, and the
  // three pieces this cycle added at THIS layer are the ones the engine
  // deliberately does not own — the config-file path (a process concern),
  // the two starter blobs, and `config validate`'s four-step rule set.
  // 53 before task 6262 ported ONE leaf out of it — `audit trail`. See the
  // task-6262 note above for why its predicted blocker was not real.
  //
  // 47 before task 6110 ported ONE leaf out of it — `workspace routing
  // show` — leaving 46. One leaf, and the scope cut is the finding: the
  // `workspace` family's other three unported leaves each carry a distinct
  // blocker (`routing build` on size, `regenerate` on an unvendored xxh64,
  // `init` on the absent layer-3 cmd surface), and `synthesize`, the other
  // half of this task, needs ~575 lines of shared `llm` + `operatorpath` +
  // `forwardspec` infrastructure that does not exist here yet AND has one
  // arm (`--literal`) blocked on the unported `import`.
  //
  // `routing show` was the one leaf in that set with NO blocker, and the
  // previous cycle's note that it should be deferred WITH `routing build`
  // did not survive checking: `show` decodes routing-table.json off disk
  // and never calls the builder. Argued in full in
  // src/lib/engine/workspace/routing.cppm's header.
  // 47 before task 6272 ported ONE leaf out of it — `workflow run` — with
  // the layer-1 `planar.process` spawn seam it had been deferred on since
  // task 6105. That leaves 46, and the count is the WHOLE story of that
  // cycle, which is why it is worth reading here rather than only in
  // src/lib/process/CMakeLists.txt.
  //
  // FIVE leaves named "a process-spawn seam" as their blocker. The seam now
  // exists at layer 1 and moved exactly ONE of them:
  //   workflow run     MOVED. `catalog::find` was already ported and shared
  //                    with `workflow show` (`not_found_error` is one
  //                    function serving both, byte-identical). The spawn
  //                    genuinely was the only missing piece — 167 zig lines
  //                    of handler and no engine half at all.
  //   capture commits   NOT moved. Needs ~1100 unported lines: the git-WALK
  //                    half of `runtime/sessioncommits.zig`. The C++
  //                    `sessioncommits` module is deliberately one pure-SQL
  //                    function (`list_for_sessions`, carved out for `audit
  //                    trail`) and says so in its own header.
  //   bench harvest    NOT moved. Needs ~671 unported lines
  //                    (`runs/harvest.zig`); no C++ equivalent exists.
  //   audit commits    NOT moved, and NOT spawn-blocked at all — this
  //                    inventory's own note above (and task 6262's) lumped
  //                    it with the 1205-line git-walk blocker. It actually
  //                    needs `listFiltered` + `writeJson`/`writeJsonList`,
  //                    ~130 zig lines, of which `listFiltered` is PURE SQL.
  //                    Cheaper than its comment claims; recorded here so
  //                    whoever picks it up does not re-scope it as a git
  //                    walk.
  //   closure compute  NOT moved. Its blocker was never the spawn — it is
  //                    tree-sitter, which is still not vendored.
  //
  // That is the second time this milestone a predicted unblock collapsed on
  // contact (task 6258 predicted eight and moved one). The pattern both
  // times: several leaves named the same MISSING THING from outside, and
  // the thing they were each actually waiting on was different.
  //
  // Three other things the cycle established, each contradicting a comment
  // that was in this tree:
  //   - "There is no process-spawn seam anywhere in this tree" was false
  //     THREE times over when task 6272 acted on it. `planar.git::run`
  //     (layer 1, popen), `editor::spawn_inherit` (layer 3, fork/execv,
  //     inherited stdio, injected env — structurally exactly what `workflow
  //     run` needed) and `ext_adapter_factory::spawn_capture` (layer 3,
  //     posix_spawnp) all existed. What was missing was a runner at a layer
  //     every consumer can reach, named for processes rather than for the
  //     first caller that needed one.
  //   - `groups recommend --solver mtkahypar` was on the blocked list and is
  //     NOT in this inventory at all: it has been wired since task 6189, and
  //     its degradation is reported rather than refused.
  //   - The worktree gate's "needs a git-subprocess seam that does not
  //     exist" note in src/cmd/planar/CMakeLists.txt outlived the seam by
  //     two tasks. Corrected there.
  // 45 before task 6278 ported `tree`, the hierarchical drill-down verb
  // and the largest single unblocked leaf left (1284 non-test Zig lines).
  // Named below rather than trusted to the count, same reason as every
  // batch above it.
  CHECK_FALSE(unported.contains("tree"));
  //
  // 45 before task 6279 moved `assoc list` and `assoc remove`. Named
  // rather than trusted to the delta: `remove` was handler-only, `list`
  // needed a ten-line kind filter beside the already-present `list_all`
  // plus the two list renderers. `assoc detect` deliberately did NOT move
  // — it is the ~680-line proposal engine and shares no code with them.
  //
  // 43 after task 6279 and 42 after task 6277 moved `audit commits`. That
  // leaf had been counted among the git-walk-blocked group here since the
  // inventory was written; the grouping was wrong (see below) and the
  // correction is why the count moved twice in one cycle.
  //
  // 41 after task 6277, and 38 after task 6294 moved the THREE `sync`
  // write leaves — `sync pull`, `sync push`, `sync resolve`. That task was
  // briefed as "six leaves all blocked on the create/propagate half of
  // `engine_extsync`"; for these three the premise was simply false. None
  // of the four oracle files behind them contains the token `extsync`.
  // They call `engine.external.sync`, a different module already ported in
  // full, so the cycle cost handler wiring and one cmd-layer helper. This
  // is the same failure mode `audit commits` hit above (a LEAF's
  // dependencies inferred from its MODULE's) in a new disguise: here it was
  // two engine modules whose NAMES look alike.
  //
  // 35 after task 6299 moved `promote`, `demote` and `test-spec status` —
  // the first three of the TEN leaves that had never been examined at all
  // this milestone. All three were handler-only, and for once the reason is
  // boring rather than a corrected mis-grouping: `planar.engine.promotion`
  // (task 6094) and `planar.engine.planning.test_spec_status` had each been
  // ported in FULL, renderers included, long before any handler existed to
  // call them. Nothing about that was inferrable from the leaves' names;
  // it came from grepping for the SYMBOLS their oracle handlers call.
  // (`test-spec status` in particular reads as a `spec ingest` sibling and
  // was not one at the time: `spec ingest` stayed unported below until
  // task 6365 — see that task's note further down.)
  //
  // The other seven of the ten did NOT move, and the split is the cycle's
  // real product. See the per-leaf notes below.
  for (auto const& leaf : {"promote", "demote", "test-spec status"}) {
    INFO("moved by task 6299: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  // The seven task 6299 probed and did NOT move, each for its own measured
  // reason. Pinned as PRESENT so a later cycle cannot wire one off the back
  // of this one's count without saying so:
  //
  //   link                       NOT engine-blocked. `external::link::create`,
  //                              `external::system::show_by_slug` and all
  //                              three `*_from_text` enums are present. Its
  //                              blocker is one FLAG: `--propagate` calls the
  //                              `ext propagate` handler, which is unported,
  //                              and the oracle's own header comment claiming
  //                              the flag "refuses with NotImplemented" is
  //                              STALE — the code runs the propagation. A
  //                              port must either land `ext propagate` or
  //                              pin a divergence on that flag, and neither
  //                              belongs in a probe cycle.
  //   workbench edit             NOT blocked either, and this was the
  //                              cycle's near-miss: `editor::resolve_editor`
  //                              + `editor::spawn_inherit` and
  //                              `workbench::sync::{push,pull}` all exist,
  //                              so it is genuinely handler-only. It is held
  //                              back only because its contract is an
  //                              INTERACTIVE editor round trip whose oracle
  //                              capture needs a non-interactive editor
  //                              stand-in, which is a test-design question
  //                              rather than a port.
  //   workbench extract-questions  Engine deps all present
  //                              (`workbench::parse::parse`,
  //                              `sync::feature_dir_for`, `root`). What is
  //                              missing is ~200 lines of PURE text
  //                              extraction (the `## Open Questions` section
  //                              walk, the H3-vs-bullet branch, the
  //                              first-sentence split) that lives in the
  //                              oracle's HANDLER and belongs in the engine
  //                              layer here. Cheap, but it is new code with
  //                              its own captures, not wiring.
  //   workbench publish          GENUINELY BLOCKED, and the only one of the
  //                              three workbench leaves that is. It calls
  //                              `engine.extsync.parent_issue.recordLink`
  //                              and `ext/remote.createRemote`;
  //                              `extsync/parent_issue.zig` is listed
  //                              unported in this tree's own
  //                              engine/extsync/CMakeLists.txt. Contradicts
  //                              the plausible reading that "the workbench
  //                              engine is fully ported" settles all three.
  //   feedback triage list|show|set  The one family with NO engine here at
  //                              all: `planar.engine.planning.feedback_triage`
  //                              did not exist in this tree (302 Zig lines,
  //                              four enums, `parse_ref`/`entity_scope`/
  //                              `set`/`show`/`list` plus two renderers).
  //                              The `feedback_triage` TABLE is present
  //                              (migration 00028), so this was an engine
  //                              port, not a schema one. Their handlers are
  //                              9/20/21/27 lines — the smallest in the
  //                              inventory — which is exactly why sizing
  //                              this family by its handlers would have been
  //                              wrong. MOVED at task 6303; see below.
  // `link` also left this list, at task 6301 — see the note further below.
  // `workbench publish` MOVED at task 6335 — its 6299-era probe had sized it
  // against `parent_issue.zig` as a whole, where the real reach was one
  // 36-line SQL function.
  INFO("moved by task 6335: workbench publish");
  CHECK_FALSE(unported.contains("workbench publish"));
  // TASK 6303 MOVED THE THREE `feedback triage` LEAVES, and the block above
  // sized them correctly: the engine was the whole job and the handlers were
  // trivia. What that sizing did NOT capture, and what actually cost the
  // cycle, is the FIXTURE. `findingPlan` compares the finding's plan slug
  // against the literal `planar-feedback`, so every arm of this family
  // refuses with `DifferentFeedbackPlan` against an ordinary plan — and a
  // bare registered project cannot even create a plan (exit 5 until the
  // project joins an association). `AmbiguousFeedbackPlan` is reachable only
  // through a QUESTION carrying two `derives-from` plan links, and is not
  // reachable for a task at all.
  for (auto const& leaf : {"feedback triage list", "feedback triage show", "feedback triage set"}) {
    INFO("moved by task 6303: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  // TASK 6302 MOVED THE TWO WORKBENCH LEAVES the block above had held back,
  // and the reasons it recorded for holding them turned out to be the
  // reasons they were cheap:
  //
  //   workbench edit               The "interactive editor round trip" was a
  //                                test-design question, and it was already
  //                                answered — the drafting quartet's
  //                                `PLANAR_EDITOR` stub-script pattern
  //                                applies unchanged. The one thing that
  //                                does NOT transfer is the argv witness:
  //                                this leaf hands the editor the FEATURE
  //                                DIRECTORY, not a temp file.
  //   workbench extract-questions  The ~200 lines of pure text walk landed
  //                                in `planar.engine.workbench.questions`
  //                                rather than the handler, matching where
  //                                every other workbench leaf's pure half
  //                                lives.
  //
  // `workbench publish` stays above, and is now the ONLY blocked leaf in
  // the family rather than one of three.
  for (auto const& leaf : {"workbench edit", "workbench extract-questions"}) {
    INFO("moved by task 6302: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  //
  // `link` LEFT this list at task 6301, which took the decision task 6299
  // deferred. The base verb was portable exactly as measured; the
  // `--propagate` flag is refused at exit 64 on the `touches_not_implemented`
  // precedent, BEFORE the link row is written so the refusal's own
  // "re-run without --propagate" advice still works. That divergence is
  // recorded in handlers/link.cppm and asserted below.
  // (The duplicate of the loop above, which task 6303 reduced to the one
  // leaf that is still blocked. Kept rather than deleted because it sits
  // after the `link` note and reads as that note's precondition.)
  // `workbench publish` MOVED at task 6335 — its 6299-era probe had sized it
  // against `parent_issue.zig` as a whole, where the real reach was one
  // 36-line SQL function.
  INFO("moved by task 6335: workbench publish");
  CHECK_FALSE(unported.contains("workbench publish"));
  INFO("moved by task 6301, with --propagate refused as a recorded divergence: link");
  CHECK_FALSE(unported.contains("link"));
  // The flag's blocker is still present, which is what makes the divergence
  // a deferral rather than a gap: both propagate leaves stay unported.
  // `link --propagate` calls the `ext propagate` handler specifically, NOT
  // `propagate-one`, so the divergence survives task 6335 moving the latter.
  INFO("`link --propagate` waits on this: ext propagate");
  CHECK(unported.contains("ext propagate"));
  INFO("moved by task 6335, and NOT what `link --propagate` calls: ext propagate-one");
  CHECK_FALSE(unported.contains("ext propagate-one"));
  // 36 after task 6298 moved TWO of the eight leaves it was handed:
  // `sync status` and `plan descendants`. The other six stayed, and the
  // reason each stayed was measured rather than assumed — see the block
  // below this CHECK.
  // 33 before task 6302; 31 after it moved `workbench edit` and
  // `workbench extract-questions`.
  // 33 before task 6309, which moved THREE: `plan next` and `ext create`
  // (both named as blocked by task 6298's probe and both sized correctly),
  // plus `link` (task 6301, minus its `--propagate` flag). 33 - 3 = 30.
  // 28 before task 6303, which moved the three `feedback triage` leaves
  // (`list`, `show`, `set`) together with the
  // `engine.planning.feedback_triage` engine that was the whole of what
  // blocked them. 28 - 3 = 25.
  // 28 before task 6310, which moved TWO — `plan divergence` and `plan
  // recommend-strategy` — in ONE cycle, because they share ~300 lines of
  // loader substrate in the oracle's `strategy.zig` and could not honestly
  // be split. 28 - 2 = 26.
  // 23 before task 6317, which moved exactly ONE: `plan closeout`, the last
  // unported leaf of the `plan` family and the only WRITING verb of the four
  // task 6298 measured. Its sibling on that task's brief, `task touches
  // infer`, was deliberately NOT attempted — see the block below. 23 - 1 = 22.
  // 23 before task 6275 moved ONE leaf out of it — `workspace routing
  // build`, the WRITE half of the routing-table family whose READ half went
  // at task 6110. 23 - 1 = 22.
  //
  // One leaf, and the scope cut is the finding, exactly as it was at 6110.
  // Task 6275 was scoped as "the four deferred workspace leaves"; three of
  // them are blocked and only this one was not, so the honest cycle is one
  // leaf plus the reasons the other three stayed. Those reasons were
  // re-checked rather than inherited:
  //   routing build  NOT blocked. 1410 lines, SQLite + filesystem, no new
  //                  dependency and no spawn seam. Size was the whole of it,
  //                  and the estimate held.
  //   regenerate     BLOCKED on an unvendored xxh64 (`.manifest-docs`
  //                  merkle) plus a hand-rolled template engine. Vendoring
  //                  is its own change under the pinned-release-archive
  //                  rule, so it is a prerequisite TASK, not a step of this
  //                  one.
  //   init           BLOCKED at layer 3, and STRICTLY LESS SO than before:
  //                  it composes scan + registration + routing build +
  //                  regenerate + symlinks, and one of those four now
  //                  exists.
  //   synthesize     BLOCKED twice over — ~575 lines of absent `llm` /
  //                  `operatorpath` / `forwardspec`, and a `--literal` arm
  //                  delegating to the unported `import`.
  // 21 before task 6325 moved `assoc detect`, the last `assoc` leaf.
  // 21 before task 6324, which moved exactly ONE: `task packet`, the last
  // big unblocked leaf of the `task` family. Task 6298 had verified it
  // BLOCKED on `engine/routing/packet.zig`'s 1674 lines, and that sizing was
  // right about the size and wrong about the block — the leaf needs only the
  // module's TASK half, and the PLANNING half it shares a file with belongs
  // to `models resolve`, which stays deferred. 21 - 1 = 20.
  //
  // Its port did NOT create the `engine_routing` bucket the Zig directory
  // layout suggests, because that bucket cannot be built: the packet's
  // freshness computation is defined in terms of `materialize`'s digests and
  // D15/D18 FATAL on a layer-2-to-layer-2 edge. It landed in `engine_ingest`
  // instead — see src/lib/engine/ingest/CMakeLists.txt for why that is the
  // honest placement rather than a workaround, and why D19's
  // extract-to-layer-1 remedy was measured and rejected.
  //
  // 19 before task 6330 ported `task touches infer`, COMPLETING the
  // `task touches` family (add / infer / list / remove). It was deferred as
  // "773 lines of git-diff and language-aware path inference" and running
  // the oracle showed that description to be wrong on both counts — it
  // shells nothing and knows no languages. 19 - 1 = 18.
  INFO("moved by task 6330, completing the `task touches` family: task touches infer");
  CHECK_FALSE(unported.contains("task touches infer"));
  // 19 before task 6329 moved TWO out of it — `dashboard` and `audit
  // handoff-readiness` — leaving 17. That task's brief was a PROBE of the
  // last five leaves nobody had examined, and the probe is the finding:
  //
  //   dashboard                MOVED. Its recorded blocker ("the absent
  //                            layer-3 cmd surface", task 6102) was
  //                            removed by task 6105 and the note was never
  //                            revisited. Every engine symbol it needs was
  //                            already here. 275 zig lines, no engine work.
  //   audit handoff-readiness  MOVED. Recorded in handlers/audit.cppm as
  //                            "merely LARGE"; it is 101 zig lines, the
  //                            SMALLEST leaf in its family, over
  //                            `resumecheck` alone.
  //   report                   NOT moved, and genuinely blocked: it needs
  //                            `engine/introspect.zig` (1330 lines) plus
  //                            `engine/introspection_adapters.zig` (1319).
  //                            Neither has any C++ equivalent. Its stated
  //                            layer-3 blocker was ALSO stale — the real
  //                            one is 2649 unported engine lines.
  //   explore                  NOT moved, and the most blocked leaf in the
  //                            inventory. The handler is 93 lines, which
  //                            is why it reads cheap; it launches the
  //                            COCKPIT, 33,452 zig lines under
  //                            cmd/planar/cockpit/ with no C++ counterpart
  //                            at all. Its non-TTY arm falls back to help
  //                            and would port in an afternoon — porting
  //                            only that arm would make the verb answer
  //                            `exit 0 + help text` on a TTY, which is the
  //                            one outcome the leaf exists to avoid.
  //   models resolve           STILL NOT MOVED, but two of its three
  //                            blockers were removed by task 6111.
  //
  //                            History, because the size estimate moved
  //                            TWICE in opposite directions. Task 6324
  //                            recorded only the planning half of
  //                            `routing/packet.zig` (~270 lines). Task 6329
  //                            corrected that UPWARD to ~1320, having found
  //                            that `handleResolve` also calls
  //                            `routing.roles` (325) and
  //                            `routing.profile.compile` (726) — the one
  //                            UNDER-statement this milestone saw.
  //
  //                            Task 6111 then measured the ~1320 and found
  //                            it ~25% high, for a reason worth keeping:
  //                            those are WHOLE-FILE line counts, and a Zig
  //                            file carries its tests inline. roles.zig is
  //                            202 implementation + 123 `test`; profile.zig
  //                            is 507 + 219. A C++ port writes its own
  //                            fixtures, so only the 709 implementation
  //                            lines transfer.
  //
  //                            Both LANDED as `engine/models/{profile,
  //                            roles}` — NOT `engine/ingest`, because their
  //                            vocabulary is `tier`/`work_type`/
  //                            `complexity`, which already live in
  //                            engine_models. Each names its own input view
  //                            instead of consuming `engine_ingest`'s
  //                            `evidence`, so no layer-2-to-layer-2 edge
  //                            exists and no D19 taxonomy extraction was
  //                            needed. See engine/models/profile.cppm.
  //
  //                            What is genuinely left is the SMALLEST of the
  //                            three: `assemble_planning` +
  //                            `compile_planning` (~300 implementation
  //                            lines), which must live in engine/ingest
  //                            beside the task half it shares digest and
  //                            canonicalization helpers with, plus the cmd
  //                            wiring. That module cannot call
  //                            engine_planning's already-ported
  //                            `test_spec_status::compute`; it must
  //                            duplicate the three counting queries the way
  //                            engine_grouping duplicates its `closures`
  //                            read.
  //
  // The two that moved are the two whose blockers were stale. The three
  // that stayed each have a real one, and `report` and `explore` were
  // parked under the SAME stale layer-3 note that `dashboard` was — so
  // that note was wrong about one leaf and accidentally right about two.
  // 16 -> 14 at task 6335: `ext propagate-one` and `workbench publish` both
  // moved. Neither needed the create/propagate half of `engine_extsync` it
  // was carried under — measured by SYMBOL, `propagate-one` reaches two
  // functions (~40 lines) and `workbench publish` reaches one (36 lines), and
  // NOTHING reaches `parent_issue.zig`'s or `projects_v2.zig`'s 2394 lines.
  // The four leaves that task 6335 was briefed to unblock split three ways:
  // two moved, `ext propagate` genuinely needs the bulk and stays, and
  // `audit publish-decision` needed ZERO of those 3665 lines but was blocked
  // on something else entirely — `postComment` on both adapters, which no
  // ported verb had ever needed. See surface.cpp's entries for all three.
  //
  // 14 -> 13 at task 6339: `audit publish-decision` moved, closing the
  // `audit` family. `postComment` landed on both adapters (~81 lines, a
  // fifth adapter-specific method outside the four-operation
  // `external_adapter` interface) plus the 176-line handler. `ext propagate`
  // — the ONE thing left carrying the bulk of `engine_extsync`'s unported
  // lines — is untouched by this move: its own caller of `postComment`
  // (`parent_issue.zig`'s injected `postCommentFn`) is a separate wiring
  // this task did not need.
  // 13 -> 12 at task 6343: `models resolve` moved, the `models` family's
  // fourteenth and last leaf. See surface.cpp's entry for the sizing note
  // (the inherited ~300-line estimate UNDER-stated it) and the two oracle
  // behaviors this port must not normalize away.
  // 12 -> 11 at task 6352: `report` moved, once `engine_introspect` (task
  // 6121) and `engine_introspection_adapters` (tasks 6102 and 6352) were
  // both complete and decision 981's layer-1 `introspection_preview`
  // extraction let `bundle::preview` reach across the D15-forbidden
  // `engine_* -> engine_*` gap between them. `explore` — the OTHER leaf
  // task 6329 probed and parked under the same stale layer-3 note `report`
  // was — stays; it needs the COCKPIT (33,452 unported zig lines), a
  // wholly different blocker `report`'s port did nothing to remove.
  // 11 -> 10 at task 6357: `health` moved, closing the family (task 6090
  // had already landed `health hygiene`). The blocker here was genuine,
  // unlike most of this milestone's over-stated ones: the handler folds
  // `engine.installedsurface.status` (548 Zig lines of manifest-driven
  // filesystem classification) into every run, and what unblocked it was
  // the SAME decision-981 shape `report` used at task 6352 — the classifier
  // ported straight to layer 1 (`planar.installed_surface`) rather than a
  // same-layer `engine_health -> engine_<classifier>` edge, since it holds
  // no `db` edge of its own.
  // 10 -> 9 at task 6358: `capture commits` moved. It had been carried as
  // blocked on 1205 lines of git-subprocess walking with "no process-spawn
  // seam in this tree" as the reason; tasks 6128/6137 had already closed
  // that seam (`planar.git`) for two OTHER consumers, and this task reached
  // it a second hop out through `sessioncommits.cppm`'s new strict git-walk
  // functions, added to the engine_runtime target `capture` already lived
  // in. See surface.cpp's entry for the full note.
  // 9 -> 8 at task 6362: `bench harvest` moved, the leaf `capture commits`'
  // note above left carried as "the other two stay". By the time this task
  // landed, the layer-1 `planar.git` seam (tasks 6128/6137) already served
  // four other consumers and was the ONLY thing this leaf was still
  // missing — its own engine half is two git subcommands plus the
  // already-ported `touch_idempotent` primitive. See
  // `src/lib/engine/runs/harvest.cppm` and surface.cpp's entry for the
  // full account.
  // 7 -> 6 at task 6365: `spec ingest` moved. Its brief carried the
  // now-familiar hypothesis that this is handler wiring over an
  // already-ported engine, true for PREVIEW mode and wrong for `--apply`:
  // `engine_ingest`'s own CMakeLists.txt documented `apply.zig` (1616 Zig
  // lines) as a genuine architectural non-port, since it composes SIX
  // layer-2 `engine_*` peers D15/D18 forbid another layer-2 bucket from
  // reaching. What that note got wrong was a stale premise (three of the
  // six callees "do not exist yet" — all three had since landed), not the
  // architecture; the fix it already named — land the composition at
  // LAYER 3, the D20 shape `annotate add` and `unlink` pioneered — is what
  // this task did. `handlers/spec_ingest.cpp` owns one outer transaction
  // for an apply; the re-entrant `planar.db` transaction seam makes each
  // composed CRUD operation a nested savepoint, preserving the oracle's
  // all-or-nothing write contract.
  CHECK(unported.size() == 6);
  // `workspace regenerate` had already moved at task 6364. It had been carried
  // as blocked on an unvendored xxh64 for its `.manifest-docs` merkle —
  // verified TRANSITIVELY true (the leaf's own source has no xxh64
  // reference; it reaches one hop out through `manifest.build`) rather than
  // stale. xxHash 0.8.3 is now vendored (`cmake/dependencies.cmake`) behind
  // the new layer-1 `planar.docs_manifest` module, and the leaf's
  // hand-rolled template engine was ported alongside it. See
  // `planar.engine.workspace.regenerate`'s header for the full account.
  CHECK(unported.size() == 6);
  INFO("moved by task 6364: workspace regenerate");
  CHECK_FALSE(unported.contains("workspace regenerate"));
  INFO("moved by task 6362: bench harvest");
  CHECK_FALSE(unported.contains("bench harvest"));
  INFO("moved by task 6365: spec ingest");
  CHECK_FALSE(unported.contains("spec ingest"));
  INFO("moved by task 6357: health");
  CHECK_FALSE(unported.contains("health"));
  INFO("moved by task 6358: capture commits");
  CHECK_FALSE(unported.contains("capture commits"));
  INFO("moved by task 6339: audit publish-decision");
  CHECK_FALSE(unported.contains("audit publish-decision"));
  INFO("moved by task 6329: dashboard");
  CHECK_FALSE(unported.contains("dashboard"));
  INFO("moved by task 6329: audit handoff-readiness");
  CHECK_FALSE(unported.contains("audit handoff-readiness"));
  INFO("moved by task 6343: models resolve");
  CHECK_FALSE(unported.contains("models resolve"));
  INFO("moved by task 6352: report");
  CHECK_FALSE(unported.contains("report"));
  INFO("moved by task 6357: health");
  CHECK_FALSE(unported.contains("health"));
  // Probed by task 6329 and deliberately NOT moved. Pinned per leaf so a
  // later cycle cannot wire one off the back of this cycle's count.
  for (auto const& leaf : {"explore"}) {
    INFO("probed by task 6329 and blocked for a NAMED reason: " << leaf);
    CHECK(unported.contains(leaf));
  }
  INFO("moved by task 6324, completing the `task` family's big unblocked leaf: task packet");
  CHECK_FALSE(unported.contains("task packet"));
  INFO("moved by task 6275: workspace routing build");
  CHECK_FALSE(unported.contains("workspace routing build"));
  // Its three family siblings stayed, and each is pinned so a later cycle
  // cannot wire one off the back of this one's count without saying so.
  for (auto const& leaf : {"workspace init", "synthesize"}) {
    INFO("probed by task 6275 and deliberately not moved: " << leaf);
    CHECK(unported.contains(leaf));
  }
  INFO("moved by task 6364: workspace regenerate");
  CHECK_FALSE(unported.contains("workspace regenerate"));
  for (auto const& leaf : {"sync pull", "sync push", "sync resolve"}) {
    INFO("moved by task 6294: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  // `sync status` — the family's fourth leaf — moved at task 6298. Task
  // 6294 had deliberately left it because it renders a listing shape none
  // of its three siblings produces and takes `--entity` rather than a
  // positional. Both remained true; what made it cheap is that its ENGINE
  // half (`sync::status` + `link::list_filter`) had shipped with the module
  // all along, so the leaf needed rendering and no engine work.
  INFO("moved by task 6298: sync status");
  CHECK_FALSE(unported.contains("sync status"));
  // `plan descendants` moved at task 6298 for the third instance of the
  // same correction `audit commits` and the `sync` trio each produced: it
  // was carried as blocked on the create/propagate half of
  // `engine_extsync`, but the LEAF needs only `walkTree` — 78 lines of
  // three SQL queries reaching no adapter, transport, credential or
  // template. It landed in `engine_planning` rather than `engine_extsync`
  // because that bucket's stated invariant is that it has NO `db` edge.
  INFO("moved by task 6298: plan descendants");
  CHECK_FALSE(unported.contains("plan descendants"));
  // The six task 6298 did NOT move, each with the blocker that was
  // VERIFIED for it rather than inherited from the brief. These are
  // asserted present so that a later cycle claiming one of them has to
  // delete the line and say why.
  //
  //   plan divergence         }  both on `engine/planning/strategy.zig`.
  //   plan recommend-strategy }  `divergence` needs only one entry point,
  //                           but it and `recommendWith` share ~300 lines
  //                           of loader substrate (`loadOpenTasks`,
  //                           `loadTouches`, `loadClosureTouches`), so the
  //                           two belong to ONE cycle, not two halves.
  //   task packet             `engine/routing/packet.zig`, 1674 lines —
  //                           the subsystem `engine/models/CMakeLists.txt`
  //                           already defers `models resolve` against.
  //                           LEFT this list at task 6324. 6298's sizing was
  //                           right about the line count and wrong about the
  //                           block: the LEAF needs only the file's TASK half
  //                           (assemble/compile/canonical/render), and the
  //                           PLANNING half — which is what `models resolve`
  //                           actually rests on, together with `roles.zig` and
  //                           `profile.zig` — was left untouched. `models
  //                           resolve` is therefore still deferred and still
  //                           for its own reasons.
  //
  // `plan next` and `ext create` were BOTH on this list and both left it at
  // task 6309, each for the reason task 6298 measured:
  //
  //   plan next    `agentactivity::next_work`, ~120 self-contained lines,
  //                named as unported in agentactivity.cppm's own header.
  //                Sized correctly and landed as sized.
  //   ext create   two `adapter_handle` accessors plus the `remote.zig`
  //                POST path, and ZERO of `engine_extsync`'s unported
  //                lines. See handlers/ext.cppm.
  // (`task packet` was asserted PRESENT here until task 6324 ported it; the
  // CHECK_FALSE that replaced it sits with the count above, where the
  // arithmetic that has to agree with it lives.)
  // `plan closeout` was the fourth of that group and LEFT this inventory at
  // task 6317, which completes the `plan` family — it has no unported leaf
  // left. 6298 sized it right: 913 lines, entirely absent, and the only one
  // of the four that WRITES (`plans.status` plus an `audit_log` row, in one
  // transaction that rolls the status change back if the audit INSERT
  // fails).
  //
  // Its cycle deliberately left `task touches infer`, the other leaf on the
  // same brief, untouched. That was the brief's own instruction ("land one
  // completely rather than both partially") and it was the right call:
  // `closeout` alone needed five distinct oracle probe arms — empty, open
  // task, open descendant plan, live claim, stale claim — plus a
  // locality-bearing fixture with a real git repository to reach the
  // advisory layer at all. `task touches infer` waited one cycle and landed
  // at task 6330 — and it needed NO git fixture at all, because
  // `touchinfer.zig` shells nothing.
  INFO("moved by task 6317, completing the `plan` family: plan closeout");
  CHECK_FALSE(unported.contains("plan closeout"));
  // Named individually rather than trusted to the count above: this file's
  // own rule is that a multi-leaf move must assert each leaf, because a
  // count that happens to balance hides a leaf moved by accident and a leaf
  // left behind.
  for (auto const& leaf : {"plan divergence", "plan recommend-strategy"}) {
    INFO("moved TOGETHER by task 6310 (shared strategy.zig loader substrate): " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  for (auto const& leaf : {"plan next", "ext create"}) {
    INFO("moved by task 6309: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  for (auto const& leaf : {"assoc list", "assoc remove"}) {
    INFO("moved by task 6279: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  INFO("moved by task 6277 after the git-walk grouping was corrected: audit commits");
  CHECK_FALSE(unported.contains("audit commits"));
  // Task 6279 deliberately LEFT this leaf; task 6325 moved it, completing
  // the `assoc` family. Named individually per this file's own rule rather
  // than trusted to the count dropping by one.
  INFO("moved by task 6325, completing the `assoc` family: assoc detect");
  CHECK_FALSE(unported.contains("assoc detect"));
  // The leaf task 6272 moved, named rather than trusted to the count. The
  // four leaves that were predicted to move WITH it must stay unported —
  // wiring any of them off the back of this cycle would claim an engine
  // half that does not exist.
  CHECK_FALSE(unported.contains("workflow run"));
  // `audit commits` WAS in this list and should never have been: it was
  // grouped with the genuinely spawn-blocked leaves on the strength of its
  // MODULE's dependencies rather than its own handler's, which calls
  // `listFiltered` + `writeJsonList` and spawns nothing. Task 6272
  // corrected the grouping and 6277 ported it. `capture commits` DID
  // really need the walk — until task 6128/6137 closed the process-spawn
  // seam for two other consumers and task 6358 reached it a second hop
  // out; see this file's own `11 -> 10` entry above. `bench harvest` was
  // this group's last member and left it at task 6362, once the seam it
  // was actually blocked on (not `sessioncommits.cppm`'s walk — that was
  // `capture commits`' blocker, not this leaf's) had a fifth consumer
  // available to reach it through.
  //
  // 76 before task 6190 ported the whole SIX-leaf `templates` family out of
  // it: `list`, `show`, `render`, `validate`, `init`, `path`. Named per
  // leaf below rather than trusted to the count, same reason as every
  // batch above it.
  //
  // Unlike the previous batches, what blocked this family was half an
  // ENGINE rather than a cmd-layer module. The RESOLUTION half (the
  // three-level fallback chain, the two enumerators) had been ported since
  // task 6032 and lives in `engine_config`; the RENDERING half did not
  // exist at all. Worth reading before touching it:
  //   - `templates render` prints the template's own KEY ORDER, so the
  //     port carries a hand-rolled insertion-ordered JSON DOM. Glaze's
  //     `json_t` is `std::map`-backed and would have re-sorted every
  //     rendered payload — valid JSON, identical values, different bytes,
  //     exit 0, invisible to every lane. See `engine/templates/jsonval.cppm`.
  //   - `{{if .X}}` on a value longer than 128 BYTES fails the whole render
  //     with `OutOfMemory` at exit 1. That is the ORACLE's behaviour (zig's
  //     `evalTruthy` uses a 128-byte stack buffer), captured at exactly 128
  //     pass / 129 fail, and reproduced deliberately. It is a real Planar
  //     defect and needs its own task against the oracle.
  //   - `templates path` ignores `--system`, `--set` AND `--json`;
  //     `templates init` ignores `--force`. Both reproduced, both captured.
  //   - Only `templates render` opens SQLite. The other five are pinned to
  //     leave `ctx.db_opened()` false.
  for (auto const& leaf :
       {"templates list", "templates show", "templates render", "templates validate", "templates init", "templates path"}) {
    INFO("templates leaf: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  // The five `scope` leaves task 6214 ported, named per leaf for the same
  // reason as every batch above. `use` / `pop` / `clear` matter most here:
  // they are REFUSALS either way, so nothing about their exit status alone
  // distinguishes the ported form (exit 2, "removed in plan 153 M5", a
  // remedy) from the unported one (exit 64, "not implemented in this
  // build"). Only membership in this inventory does.
  for (auto const& leaf : {"scope show", "scope suggest", "scope use", "scope pop", "scope clear"}) {
    INFO("scope leaf: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  // The leaf task 6258 moved, named rather than trusted to the count. The
  // three `ext` leaves BESIDE it must stay unported: the factory is a
  // necessary but nowhere near sufficient condition for them, and a port
  // that wired them off the back of this cycle would be claiming a
  // create/propagate path that does not exist.
  CHECK_FALSE(unported.contains("ext test"));
  // `ext create` was one of those three and LEFT at task 6295, which found
  // the grouping half-wrong: the factory really was insufficient for it, but
  // what it additionally needed was two `adapter_handle` accessors, NOT the
  // create/propagate half of `engine_extsync`. Its two siblings genuinely do
  // reach `propagate.zig` and stay.
  CHECK_FALSE(unported.contains("ext create"));
  // "Its two siblings genuinely do reach `propagate.zig`" was TRUE and
  // insufficient, which is why task 6335 re-measured it. `propagate-one`
  // reaches that file — for exactly two functions, ~40 lines, neither of
  // which reaches anything else in the create/propagate surface. Reaching a
  // file is not the same as needing it, and the whole `ext` family's sizing
  // rested on the conflation. MOVED at task 6335.
  CHECK_FALSE(unported.contains("ext propagate-one"));
  // `ext propagate` is the one leaf of the four that genuinely wants the
  // bulk: `selectStrategy`, `walkTree`, all of `strategy.zig`, and both
  // GitHub-specific files.
  INFO("still-deferred ext leaf: ext propagate");
  CHECK(unported.contains("ext propagate"));
  // The deliberately-deferred leaves from otherwise-ported families. They
  // must remain DECLARED (exit 64), never silently absent.
  //
  // `task touches infer` was the third of these until task 6330, which
  // ported it and completed the `task touches` family. It had been deferred
  // as "773 lines of git-diff and language-aware path inference" — a
  // description running the oracle showed to be wrong on both counts. It is
  // asserted ABSENT at the top of this test alongside the count.
  INFO("moved by task 6343: models resolve");
  CHECK_FALSE(unported.contains("models resolve"));
  // 92 before task 6205 ported SIXTEEN out of it in one change -- the
  // drafting quartet on all four link-anchored planning families, wired
  // together with the `editflow` port that four consecutive cycles had
  // deferred them on. It is the largest single batch this milestone has
  // moved, and the ONLY one where the leaves span four families, because
  // what blocked them was one shared cmd-layer module rather than any
  // family's engine.
  //
  // Named individually, and in a shape that fails PER LEAF rather than
  // per family, for the reason the count alone cannot serve: sixteen
  // leaves leaving one inventory in one commit is exactly where one of
  // them silently stays behind while the total still moves by sixteen
  // because something unrelated was dropped in its place.
  for (auto const& family : {"question", "decision", "scenario", "artifact"}) {
    for (auto const& verb : {"edit", "view", "diff", "review"}) {
      auto const leaf = std::format("{} {}", family, verb);
      INFO("drafting leaf: " << leaf);
      CHECK_FALSE(unported.contains(leaf));
    }
  }
  // The `plan` and `task` quartets -- the same eight leaves over the same
  // module -- landed at task 6208. Until then this block asserted the
  // OPPOSITE, so that wiring them without doing the oracle run would fail
  // here rather than ship silently. It did its job: the run it forced found
  // a real divergence (below), which "the module compiles for them too"
  // would have shipped wrong.
  //
  // The block is INVERTED rather than deleted, for the reason every other
  // per-leaf loop in this file exists: eight leaves leaving one inventory in
  // one commit is exactly where one silently stays behind while the total
  // still moves by eight because something unrelated was dropped with them.
  for (auto const& family : {"plan", "task"}) {
    for (auto const& verb : {"edit", "view", "diff", "review"}) {
      auto const leaf = std::format("{} {}", family, verb);
      INFO("drafting leaf, oracle-derived at 6208: " << leaf);
      CHECK_FALSE(unported.contains(leaf));
    }
  }
  // And the divergence the hold existed to surface, pinned where the next
  // reader of this file will see it: `plan` and `task` resolve their anchor
  // through `plans`/`tasks` rather than `entity_links`, so a nonexistent id
  // is `not_found` for them and `no_plan_link` for the other four. The
  // observable difference is the `diff`/`review` prose --
  //   `plan diff 999`      -> `no plan with id 999`
  //   `question diff 999`  -> `question 999 is not linked to a plan; ...`
  // -- both exit 1, so ONLY the prose separates them. Asserted end-to-end
  // in `drafting_leaves.t.cpp`; named here so a future batch that
  // "unifies" the two arms trips a test that explains why not to.
  // The CRUD/transition halves each family's own cycle ported must still
  // not be in the inventory -- the count would catch a leaf that stayed,
  // but not a leaf that stayed while a DIFFERENT one was dropped by
  // mistake, so each is named.
  for (auto const& wired :
       {"question add",  "question show", "question list",   "question answer",    "question wontfix",  "decision add",
        "decision show", "decision list", "decision accept", "decision supersede", "decision withdraw", "decision link",
        "scenario add",  "scenario show", "scenario list",   "scenario verify",    "scenario retire",   "scenario link",
        "artifact add",  "artifact show", "artifact list",   "artifact update",    "artifact link"}) {
    INFO("planning leaf: " << wired);
    CHECK_FALSE(unported.contains(wired));
  }
  // The seven task 6193 ported must NOT be in the inventory. The count
  // above would catch a leaf that stayed, but not a leaf that stayed while
  // a DIFFERENT one was dropped by mistake — so each is named.
  for (auto const& linked :
       {"links add", "links list", "links remove", "links trail", "plan link", "task link", "question link"}) {
    INFO("entity-link leaf: " << linked);
    CHECK_FALSE(unported.contains(linked));
  }
  // The five `config` leaves task 6259 ported, named individually for the
  // reason every loop above it exists: five leaving one inventory in one
  // commit is where one silently stays behind while the total still moves
  // by five because something unrelated was dropped with them.
  for (auto const& leaf : {"config show", "config edit", "config validate", "config init", "config path"}) {
    INFO("config leaf: " << leaf);
    CHECK_FALSE(unported.contains(leaf));
  }
  // ...and the fourth, from task 6189's own families. `closure show` is
  // ported; `closure compute` must stay DECLARED, never silently absent.
  CHECK_FALSE(unported.contains("closure compute"));
  // The three dual nodes (`resume`, `handoff`, `health`) ALL have real
  // handlers now that task 6357 closed the last of them — none may appear
  // in the inventory.
  CHECK_FALSE(unported.contains("health"));
  CHECK_FALSE(unported.contains("resume"));
  CHECK_FALSE(unported.contains("handoff"));
  // No implemented verb may appear in the inventory.
  for (auto const& implemented : {"init",
                                  "version",
                                  "schema",
                                  "completion",
                                  "unlink",
                                  "workbench gc",
                                  "annotate add",
                                  "capture snapshot",
                                  "handoff show",
                                  "resume validate",
                                  "ext list",
                                  "plan create",
                                  "assoc create",
                                  "task add",
                                  "assoc add",
                                  "plan show",
                                  "plan list",
                                  "plan update",
                                  "plan recompute-status",
                                  "task show",
                                  "task list",
                                  "task update",
                                  "task done",
                                  "task cancel",
                                  "task block",
                                  "task reopen",
                                  "local list",
                                  "local link",
                                  "local unlink",
                                  "local import",
                                  "local migrate",
                                  "closure show",
                                  "groups recommend",
                                  "question add",
                                  "question show",
                                  "question list",
                                  "question answer",
                                  "question wontfix",
                                  "assoc members"}) {
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
