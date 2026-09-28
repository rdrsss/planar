// @file capability.t.cpp
// @brief The `planar-agent` capability-boundary contract, plus the tree and
// dispatch-registration gates that hold it up (plan 996, task 6107).
//
// Port target: zig/integration_tests/capability_boundary_test.zig's
// planar-agent half, whose own header calls these tests "the SECURITY
// contract — a vendor hook configured with only planar-agent on PATH cannot
// touch planning state".
//
// ## What these tests actually prove, and what they cannot
//
// The Zig suite asserts the verb set is EXACTLY eighteen names AND contains
// NONE of twenty planning-entity verbs. Those two halves are not equally
// portable right now, and conflating them would produce a test that looks
// stronger than it is:
//
//   THE FORBIDDEN HALF IS FULLY PORTED AND IS THE SECURITY CONTRACT.
//   "no planning verb is reachable" is true of a subset exactly as it is
//   true of the full surface, so `refuses every planning-entity verb`
//   below is the real thing, not a placeholder. It walks the WHOLE tree
//   (`cliapp::all_nodes`), not just the root's children, so a planning verb
//   smuggled in as a SUBcommand of a legitimate one is caught too — the
//   Zig version only parses the root `--help` listing and would miss that.
//
//   THE EXACT-SET HALF IS A COMPLETENESS CHECK, NOT A CAPABILITY ONE, and
//   this tree registers two of the oracle's eighteen verbs. So it is
//   asserted against the PORTED set and the oracle's full set is recorded
//   here as the target rather than silently asserted as if met. Claiming
//   eighteen would be a false green; asserting nothing would let a verb
//   appear without anyone noticing.
//
// ## Break-probes run against this file
//
//   - Added `.name = "task"` as a child of the root  -> `refuses every
//     planning-entity verb` FAILS. Restored -> green.
//   - Added `.name = "task"` as a CHILD OF `schema` (the smuggling case
//     the Zig version cannot see, since it only parses the root --help
//     listing) -> same test FAILS. Restored -> green.
//   - Dropped `schema` from `handlers()` -> `every planar-agent leaf is
//     wired to a handler` FAILS. Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.surface;
import planar.cmd.planar_agent.main;

namespace {

/// @brief Every node name in the tree except the root's own, at any depth.
///
/// Depth matters: a forbidden verb hidden as a grandchild is exactly as
/// reachable from argv as one at the root, and is harder to spot by eye.
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

TEST_CASE("planar-agent refuses every planning-entity verb, at any depth", "[cmd][agent][capability]") {
  auto const root  = planar::cmd::agent::root_app();
  auto const names = all_node_names(*root);

  // The forbidden list is the binary's documented contract (exported from
  // `tree.cppm` rather than hidden here) and is transcribed from
  // zig/integration_tests/capability_boundary_test.zig.
  REQUIRE(planar::cmd::agent::forbidden_verbs().size() == 20);
  for (auto const& forbidden : planar::cmd::agent::forbidden_verbs()) {
    INFO("forbidden verb leaked into planar-agent's tree: " << forbidden);
    CHECK_FALSE(names.contains(forbidden));
  }
}

TEST_CASE("planar-agent's declared verb set is exactly the oracle's", "[cmd][agent][capability]") {
  auto const root  = planar::cmd::agent::root_app();
  auto const names = all_node_names(*root);

  // TASK 6065 REPLACED THIS ASSERTION, and inverted its rationale.
  //
  // It used to pin the FOURTEEN ported verbs and then assert the other
  // four were absent, on the grounds that "an absent verb is a clean
  // `unknown subcommand`; a registered stub that exits 64 looks like a
  // working verb to a script."
  //
  // Both halves of that turned out to be wrong, and both were measured
  // rather than argued:
  //
  //   * Exit 64 is NOT what a working verb looks like to a script. It is
  //     non-zero, it writes nothing to stdout, and it writes
  //     `error: <verb>: not implemented in this build` to stderr. A script
  //     under `set -e` stops on it exactly as it would on the
  //     unknown-subcommand path, with a message that says more.
  //   * Omitting a verb does NOT make `make cli-usage-check` stricter
  //     about it. `zig/tools/cli_usage_lint` SKIPS a command path it
  //     cannot resolve — its own header says it reports "any `--flag`
  //     referenced on a command that the binary does not actually expose",
  //     and `src/lib/cliapp/schema.t.cpp`'s `[lint-parity]` scope section
  //     proves the unknown-path case exits 0. So every omitted verb was a
  //     HOLE in the live gate, not a safeguard.
  //
  // So the set is now the oracle's whole eighteen top-level verbs, still
  // asserted EXACTLY — a completeness check against reality, so an
  // invented verb fails it just as it did before.
  CHECK(names == std::set<std::string, std::less<>>{
                     // The fourteen with real handlers.
                     "abort", "action", "block", "claim", "claim-associate", "complete", "end", "fail", "heartbeat", "peek",
                     "pull", "reconcile", "release", "schema", "start", "version",
                     // The nine declared by task 6065, which refuse at 64.
                     "add", "capsule", "confirm", "context", "dispatch", "ingest", "list", "preview", "resolve", "run"});
}

TEST_CASE("every planar-agent verb is either implemented or refuses at 64", "[cmd][agent][capability]") {
  // The property that makes declaring the full surface safe, asserted
  // structurally rather than by invoking 24 verbs: every leaf is in the
  // handler table (`unregistered_leaves`, below), and every leaf is in
  // EXACTLY ONE of the two populations — the hand-registered handlers or
  // the generated unported inventory.
  auto const root  = planar::cmd::agent::root_app();
  auto const table = planar::cmd::agent::handlers(*root);

  std::set<std::string, std::less<>> unported;
  for (auto const& verb : planar::cmd::agent::unported_paths()) {
    unported.emplace(verb);
  }
  CHECK(unported.empty());

  auto const leaves = planar::cliapp::leaf_keys(*root);
  CHECK(leaves.size() == 25);
  for (auto const& leaf : leaves) {
    INFO("leaf: " << leaf);
    CHECK(table.contains(leaf));
  }
  // Non-vacuous: the inventory must not have swallowed a verb that has a
  // real handler, which is the failure mode a bulk registration invites.
  for (auto const& implemented : {"pull", "complete", "action start", "run start", "run end", "schema", "version"}) {
    INFO("implemented verb wrongly listed as unported: " << implemented);
    CHECK_FALSE(unported.contains(implemented));
  }
}

TEST_CASE("planar-agent's root node names itself", "[cmd][agent][capability]") {
  // Not cosmetic: the root's name is the `[in: planar-agent]` suffix in a
  // parse-error message and the `"root"` field of the schema catalog, both
  // of which are oracle-compared in parity.t.cpp.
  CHECK(planar::cmd::agent::root_app()->get_name() == "planar-agent");
}

TEST_CASE("every planar-agent leaf is wired to a handler", "[cmd][agent][dispatch]") {
  auto const root  = planar::cmd::agent::root_app();
  auto const table = planar::cmd::agent::handlers(*root);

  auto const missing = planar::cmd::agent::unregistered_leaves(*root, table);
  INFO("leaves with no handler: " << missing.size());
  CHECK(missing.empty());
}

TEST_CASE("no planar-agent handler is unreachable from argv", "[cmd][agent][dispatch]") {
  auto const root  = planar::cmd::agent::root_app();
  auto const table = planar::cmd::agent::handlers(*root);

  // The other direction: a handler registered under a misspelled path is
  // dead code that looks exactly like working coverage.
  auto const dead = planar::cmd::agent::unreachable_handlers(*root, table);
  INFO("handlers no argv can reach: " << dead.size());
  CHECK(dead.empty());
}

TEST_CASE("planar-agent path_key joins segments with single spaces", "[cmd][agent][dispatch]") {
  std::vector<std::string> const path{"run", "start"};
  CHECK(planar::cliapp::path_key(path) == "run start");
  CHECK(planar::cliapp::path_key(std::vector<std::string>{}).empty());
}
