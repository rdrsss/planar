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
//   (`cli::all_nodes`), not just the root's children, so a planning verb
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
import planar.cli;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.tree;

namespace {

/// @brief Every node name in the tree except the root's own, at any depth.
///
/// Depth matters: a forbidden verb hidden as a grandchild is exactly as
/// reachable from argv as one at the root, and is harder to spot by eye.
/// @param root The tree to walk.
/// @return The set of names.
auto all_node_names(const planar::cli::cmd& root) -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> names;
  for (auto const& node : planar::cli::all_nodes(root)) {
    if (node.path.empty()) {
      continue;
    }
    names.insert(node.node->name);
  }
  return names;
}

} // namespace

TEST_CASE("planar-agent refuses every planning-entity verb, at any depth", "[cmd][agent][capability]") {
  auto const root  = planar::cmd::agent::root_command();
  auto const names = all_node_names(root);

  // The forbidden list is the binary's documented contract (exported from
  // `tree.cppm` rather than hidden here) and is transcribed from
  // zig/integration_tests/capability_boundary_test.zig.
  REQUIRE(planar::cmd::agent::forbidden_verbs().size() == 20);
  for (auto const& forbidden : planar::cmd::agent::forbidden_verbs()) {
    INFO("forbidden verb leaked into planar-agent's tree: " << forbidden);
    CHECK_FALSE(names.contains(forbidden));
  }
}

TEST_CASE("planar-agent's registered verb set is exactly the ported subset", "[cmd][agent][capability]") {
  auto const root  = planar::cmd::agent::root_command();
  auto const names = all_node_names(root);

  // The oracle registers eighteen: pull, peek, claim, heartbeat,
  // claim-associate, complete, fail, release, block, action, ingest,
  // reconcile, abort, version, schema, run, dispatch, context. FOURTEEN
  // are ported (task 6038 landed the claim ritual); the remaining four are
  // blocked one layer down on buckets this tree has never ported — see
  // tree.cppm's per-verb inventory. This asserts exactly the ported set
  // and nothing else: a completeness check against REALITY, not against
  // the target, so an accidentally-registered stub fails it too.
  CHECK(names == std::set<std::string, std::less<>>{"abort", "action", "block", "claim", "claim-associate", "complete", "end",
                                                    "fail", "heartbeat", "peek", "pull", "reconcile", "release", "schema",
                                                    "start", "version"});

  // And the four that are NOT here, named explicitly. An absent verb is a
  // clean `unknown subcommand`; a registered stub that exits 64 looks like
  // a working verb to a script.
  for (auto const& deferred : {"ingest", "run", "dispatch", "context"}) {
    INFO("deferred verb: " << deferred);
    CHECK_FALSE(names.contains(deferred));
  }
}

TEST_CASE("planar-agent's root node names itself", "[cmd][agent][capability]") {
  // Not cosmetic: the root's name is the `[in: planar-agent]` suffix in a
  // parse-error message and the `"root"` field of the schema catalog, both
  // of which are oracle-compared in parity.t.cpp.
  CHECK(planar::cmd::agent::root_command().name == "planar-agent");
}

TEST_CASE("every planar-agent leaf is wired to a handler", "[cmd][agent][dispatch]") {
  auto const root  = planar::cmd::agent::root_command();
  auto const table = planar::cmd::agent::handlers(root);

  auto const missing = planar::cmd::agent::unregistered_leaves(root, table);
  INFO("leaves with no handler: " << missing.size());
  CHECK(missing.empty());
}

TEST_CASE("no planar-agent handler is unreachable from argv", "[cmd][agent][dispatch]") {
  auto const root  = planar::cmd::agent::root_command();
  auto const table = planar::cmd::agent::handlers(root);

  // The other direction: a handler registered under a misspelled path is
  // dead code that looks exactly like working coverage.
  auto const dead = planar::cmd::agent::unreachable_handlers(root, table);
  INFO("handlers no argv can reach: " << dead.size());
  CHECK(dead.empty());
}

TEST_CASE("planar-agent path_key joins segments with single spaces", "[cmd][agent][dispatch]") {
  std::vector<std::string> const path{"run", "start"};
  CHECK(planar::cmd::agent::path_key(path) == "run start");
  CHECK(planar::cmd::agent::path_key(std::vector<std::string>{}).empty());
}
