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
  // It is now `dashboard`: a TOP-LEVEL leaf rather than a family member,
  // arg-free (no positionals at all in `surface.cpp`, so the parser cannot
  // refuse at exit 2 before dispatch is reached), and blocked on the
  // agent-claim roll-up its `--agents` arm needs. Being top-level is a
  // small additional guarantee — there is no sibling port that can drag it
  // along by accident, the way each planning family's CRUD half dragged
  // its predecessors.
  auto const leaf = dispatch({"dashboard"});
  CHECK(leaf.code == 64);
  CHECK(leaf.out.empty());
  CHECK(leaf.err == "error: dashboard: not implemented in this build\n");

  // Deeper, to prove the key is the full path and not the leaf name.
  //
  // This was `feedback triage list` until task 6303 ported that family, and
  // the replacement has the same two constraints the top-level exemplar
  // above has: it must be genuinely unported, and its positionals must all
  // be OPTIONAL or the parser refuses at exit 2 before dispatch is reached.
  // `task touches infer`, the only other three-level unported path, fails
  // the second test (`task-id` is required), which leaves this one.
  auto const deep = dispatch({"workspace", "routing", "build"});
  CHECK(deep.code == 64);
  CHECK(deep.err == "error: workspace routing build: not implemented in this build\n");

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
  //
  // The child asserted here is `health hygiene`, which task 6090 PORTED —
  // so what it proves is now the stronger half of the same property: the
  // parent's own exit-64 refusal does NOT swallow a working child. Before
  // 6090 this line read `child.code == 64`, and leaving it that way after
  // the port made this case the only red test in the suite. It was
  // RETARGETED rather than weakened: a `CHECK(child.code != 64)` would
  // have gone green for a child that failed some other way.
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
  // git-diff-and-language-aware path inference respectively). Wiring a
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
  // is not one: `spec ingest` stays unported below.)
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
  for (auto const& leaf : {"workbench publish"}) {
    INFO("probed but deliberately not moved by task 6299: " << leaf);
    CHECK(unported.contains(leaf));
  }
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
  for (auto const& leaf : {"workbench publish"}) {
    INFO("probed but deliberately not moved by task 6299: " << leaf);
    CHECK(unported.contains(leaf));
  }
  INFO("moved by task 6301, with --propagate refused as a recorded divergence: link");
  CHECK_FALSE(unported.contains("link"));
  // The flag's blocker is still present, which is what makes the divergence
  // a deferral rather than a gap: both propagate leaves stay unported.
  for (auto const& leaf : {"ext propagate", "ext propagate-one"}) {
    INFO("`link --propagate` waits on this: " << leaf);
    CHECK(unported.contains(leaf));
  }
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
  CHECK(unported.size() == 25);
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
  //   plan closeout           `engine/planning/closeout.zig`, 913 lines,
  //                           entirely absent here. The janitor's
  //                           authoritative close gate.
  //   plan divergence         }  both on `engine/planning/strategy.zig`.
  //   plan recommend-strategy }  `divergence` needs only one entry point,
  //                           but it and `recommendWith` share ~300 lines
  //                           of loader substrate (`loadOpenTasks`,
  //                           `loadTouches`, `loadClosureTouches`), so the
  //                           two belong to ONE cycle, not two halves.
  //   task packet             `engine/routing/packet.zig`, 1674 lines —
  //                           the subsystem `engine/models/CMakeLists.txt`
  //                           already defers `models resolve` against.
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
  for (auto const& leaf : {"plan closeout", "plan divergence", "plan recommend-strategy", "task packet"}) {
    INFO("task 6298 verified this one BLOCKED rather than assuming it: " << leaf);
    CHECK(unported.contains(leaf));
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
  INFO("task 6279 deliberately left `assoc detect` — the ~680-line proposal engine");
  CHECK(unported.contains("assoc detect"));
  // The leaf task 6272 moved, named rather than trusted to the count. The
  // four leaves that were predicted to move WITH it must stay unported —
  // wiring any of them off the back of this cycle would claim an engine
  // half that does not exist.
  CHECK_FALSE(unported.contains("workflow run"));
  // `audit commits` WAS in this list and should never have been: it was
  // grouped with the genuinely spawn-blocked leaves on the strength of its
  // MODULE's dependencies rather than its own handler's, which calls
  // `listFiltered` + `writeJsonList` and spawns nothing. Task 6272
  // corrected the grouping and 6277 ported it. `capture commits` stays —
  // it really does need the walk.
  for (auto const& leaf : {"capture commits"}) {
    INFO("still-deferred spawn-adjacent leaf: " << leaf);
    CHECK(unported.contains(leaf));
  }
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
  for (auto const& leaf : {"ext propagate-one", "ext propagate"}) {
    INFO("still-deferred ext leaf: " << leaf);
    CHECK(unported.contains(leaf));
  }
  // The three deliberately-deferred leaves from otherwise-ported families.
  // They must remain DECLARED (exit 64), never silently absent.
  CHECK(unported.contains("bench harvest"));
  CHECK(unported.contains("models resolve"));
  CHECK(unported.contains("task touches infer"));
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
  CHECK(unported.contains("closure compute"));
  // The three duals are the entries that are NOT leaves; `resume` and
  // `handoff` have real handlers, so `health` is the only one here.
  CHECK(unported.contains("health"));
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
