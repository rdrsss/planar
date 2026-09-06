// @file capability.t.cpp
// @brief LEVEL 1 of `planar-watch`'s read-only guarantee: the verb set
// (plan 996, task 6107).
//
// Port target: zig/integration_tests/capability_boundary_test.zig's
// planar-watch half — "a watcher configured with only planar-watch on PATH
// cannot touch anything at all."
//
// Level 2 (the SQLITE_OPEN_READONLY handle) is `context.t.cpp`. Neither
// subsumes the other and both are required: the verb set means no code path
// composes a mutating statement, the handle means even a mistake in the
// verb set cannot write. The binary's own `--help` page advertises exactly
// this pairing to operators.
//
// As with planar-agent, the FORBIDDEN half is fully ported and is the
// security contract; the EXACT-SET half is a completeness check asserted
// against the ported subset, with the oracle's full twelve recorded rather
// than falsely claimed. And as there, the walk is over `cliapp::all_nodes` —
// the WHOLE tree at any depth — so a write verb smuggled in as a
// subcommand is caught, which the Zig version's root-`--help` parse
// would miss.
//
// ## Break-probes run against this file
//
//   - Added `.name = "claim"` (a planar-agent write verb) to the root ->
//     `refuses every write verb` FAILS. Restored -> green.
//   - Added `.name = "task"` as a child of `completion` -> same test
//     FAILS. Restored -> green.
//   - Registered a handler under the misspelled key "verison" -> `no
//     planar-watch handler is unreachable from argv` FAILS. Restored ->
//     green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;
import planar.cmd.planar_watch.dispatch;
import planar.cmd.planar_watch.surface;
import planar.cmd.planar_watch.tree;

namespace {

/// @brief Every node name in the tree except the root's own, at any depth.
/// @param root The tree to walk.
/// @return The set of names.
auto all_node_names(const CLI::App& root) -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> names;
  for (auto const& node : planar::cliapp::all_nodes(root)) {
    if (node.path.empty()) {
      continue;
    }
    names.insert(node.node->get_name());
  }
  return names;
}

} // namespace

TEST_CASE("planar-watch refuses every write verb from both other binaries, at any depth", "[cmd][watch][capability]") {
  auto const root  = planar::cmd::watch::root_app();
  auto const names = all_node_names(*root);

  // Twelve planar-agent write verbs + seventeen planar planning-entity
  // verbs, transcribed from the Zig suite's two assertContainsNone lists.
  REQUIRE(planar::cmd::watch::forbidden_verbs().size() == 29);
  for (auto const& forbidden : planar::cmd::watch::forbidden_verbs()) {
    INFO("forbidden verb leaked into planar-watch's tree: " << forbidden);
    CHECK_FALSE(names.contains(forbidden));
  }
}

TEST_CASE("planar-watch's declared verb set is exactly the oracle's", "[cmd][watch][capability]") {
  auto const root  = planar::cmd::watch::root_app();
  auto const names = all_node_names(*root);

  // The oracle registers twelve top-level verbs: feed, ps, claims,
  // actions, plans, log, tree, run, sync-events, version, completion,
  // schema. Task 6120 landed the six read verbs that rest on
  // `engine.runtime.agentactivity`; task 6065 DECLARED three more
  // (`sync-events`, `run list`, `run show`) so that
  // `zig/tools/cli_usage_lint` can resolve them, each refusing at exit 64
  // — proved by name in parity.t.cpp. Task 6039 landed `feed` for real —
  // see handlers/feed.cppm. Declaring without landing is not implementing,
  // and this binary keeps the difference loud for what remains unported.
  //
  // THE EXACT-SET FORM IS LOAD-BEARING, not a stylistic choice. A
  // `contains` check would let a write verb in; equality means adding ANY
  // node to this binary's tree — including an innocent-looking one — has to
  // come through this line, which is a deliberate stop for a binary whose
  // whole contract is what it cannot do. That is why widening it to the
  // oracle's full surface is written out verb by verb rather than derived
  // from the generated table it is checking.
  //
  // Note `plans` and `tree` sit one character from `plan` and one word from
  // planar's own `tree` verb, and NEITHER is forbidden: `forbidden_verbs`
  // lists `plan` (the planning-entity verb), and membership is tested by
  // whole-name equality, not by prefix. `planar-watch plans` LISTS plans;
  // `planar plan` mutates them. The test above would fail on `plan` and
  // passes on `plans`, which is the distinction actually intended.
  //
  // `run` is now in the tree and is NOT in `forbidden_verbs` — the
  // forbidden list names `planar-agent`'s write verbs and `planar`'s
  // planning-entity verbs, and neither has a `run`. `planar-watch run`
  // OBSERVES workflow runs; the case above would still fail if `ingest`,
  // `pull` or `capture` appeared here.
  CHECK(names == std::set<std::string, std::less<>>{"actions", "claims", "completion", "feed", "list", "log", "plans", "ps",
                                                    "run", "schema", "show", "sync-events", "tree", "version"});
}

TEST_CASE("every planar-watch verb is either implemented or refuses at 64", "[cmd][watch][capability]") {
  // Structural companion to parity.t.cpp's behavioural case: every leaf is
  // in the handler table, and every leaf is in EXACTLY ONE of the two
  // populations — the hand-registered handlers or the generated unported
  // inventory. A bulk registration's failure mode is swallowing a verb
  // that already had a real handler, so that direction is asserted too.
  auto const root  = planar::cmd::watch::root_app();
  auto const table = planar::cmd::watch::handlers(*root);

  std::set<std::string, std::less<>> unported;
  for (auto const& verb : planar::cmd::watch::unported_paths()) {
    unported.emplace(verb);
  }
  // Task 6448 landed real handlers for the three that used to be the whole
  // contents of this set. Nothing on this binary remains declared-but-
  // unported — pinned as an EMPTY set rather than deleting the assertion,
  // so a future addition to `unported_paths()` still has a test noticing it.
  CHECK(unported.empty());

  auto const leaves = planar::cliapp::leaf_keys(*root);
  CHECK(leaves.size() == 13);
  for (auto const& leaf : leaves) {
    INFO("leaf: " << leaf);
    CHECK(table.contains(leaf));
  }
  for (auto const& implemented : {"feed", "ps", "claims", "actions", "plans", "log", "tree", "version", "schema", "completion",
                                  "run list", "run show", "sync-events"}) {
    INFO("implemented verb wrongly listed as unported: " << implemented);
    CHECK_FALSE(unported.contains(implemented));
  }
}

TEST_CASE("planar-watch's root advertises the read-only invariant to operators", "[cmd][watch][capability]") {
  auto const root = planar::cmd::watch::root_app();
  CHECK(root->get_name() == "planar-watch");
  // The description is not decoration: it is where an operator reading
  // `--help` learns the handle is SQLITE_OPEN_READONLY and that the verb
  // set is the first line of defense. Losing it in transcription would
  // quietly delete the binary's own statement of its contract.
  auto const description = root->get_description();
  CHECK(description.contains("strict read-only mode"));
  CHECK(description.contains("SQLITE_OPEN_READONLY"));
  CHECK(description.contains("no write verbs registered"));
  // Task 6123 note: `cli::cmd` carried `desc` (one-line summary) and
  // `long_desc` (the prose block) as SEPARATE fields, and this case used to
  // assert they differed. `CLI::App` carries ONE description string, so
  // the longer operator-facing block is what survives and the one-line
  // summary is gone from this binary's surface — see
  // `planar.cliapp.schema`'s header, divergence 1. That is a real, named
  // loss of the swap, recorded here rather than silently dropped: the
  // schema catalog now reports the same string for "summary" and
  // "description". Note the ORACLE's catalog could not distinguish them
  // either (its emitter falls back to `desc` when `long_desc` is empty) —
  // only the rendered help page could, via indentation.
  CHECK(description.starts_with("planar-watch is the human-facing live cockpit"));
}

TEST_CASE("every planar-watch leaf is wired to a handler", "[cmd][watch][dispatch]") {
  auto const root    = planar::cmd::watch::root_app();
  auto const table   = planar::cmd::watch::handlers(*root);
  auto const missing = planar::cmd::watch::unregistered_leaves(*root, table);
  INFO("leaves with no handler: " << missing.size());
  CHECK(missing.empty());
}

TEST_CASE("no planar-watch handler is unreachable from argv", "[cmd][watch][dispatch]") {
  auto const root  = planar::cmd::watch::root_app();
  auto const table = planar::cmd::watch::handlers(*root);
  auto const dead  = planar::cmd::watch::unreachable_handlers(*root, table);
  INFO("handlers no argv can reach: " << dead.size());
  CHECK(dead.empty());
}
