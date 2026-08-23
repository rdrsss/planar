/// @file tree.cppm
/// @brief `planar.cmd.planar_agent.tree` — the `planar-agent` binary's root
/// command tree (plan 996, task 6107).
///
/// Port target: the `root` constant in zig/src/cmd/planar-agent/main.zig
/// plus each `handlers/<verb>.zig`'s `pub const verb: cli.Cmd`.
///
/// ## THE VERB SET IS THE CAPABILITY BOUNDARY
///
/// This is not an ordinary command tree. CLAUDE.md § four-binary boundary
/// and zig/integration_tests/capability_boundary_test.zig both state the
/// invariant the same way: the capability boundary is *the binary's verb
/// set*, not runtime ACLs. A vendor hook configured with only
/// `planar-agent` on `PATH` cannot touch planning state, and the ONLY
/// thing making that true is that no planning-entity verb is registered in
/// this function. There is no second check at runtime to fall back on.
///
/// So the FORBIDDEN direction is absolute and is pinned by
/// `tree.t.cpp`: none of `plan`, `task`, `decision`, `question`,
/// `scenario`, `artifact`, `annotate`, `init`, `workbench`, `doc`, `spec`,
/// `templates`, `ext`, `sync`, `promote`, `demote`, `capture`,
/// `dashboard`, `tree`, `health` may appear here, at any depth, ever.
///
/// ## What this tree deliberately is NOT (yet)
///
/// The oracle registers eighteen verbs. This tree registers TWO — `version`
/// and `schema` — and that gap is a porting gap, not a capability
/// statement. Every remaining verb (`pull`, `peek`, `claim`, `heartbeat`,
/// `claim-associate`, `complete`, `fail`, `release`, `block`, `action`,
/// `ingest`, `reconcile`, `abort`, `run`, `dispatch`, `context`) is
/// blocked ONE LAYER DOWN, on layer-2 engine buckets this tree has never
/// ported:
///
///   pull / peek / claim / heartbeat / complete / fail / release / block /
///   abort / reconcile / claim-associate
///                          need the `agent_work_claims` + `agent_actions`
///                          store (zig `engine.runtime.agentactivity` and
///                          the atomic terminal-verb transactions). No
///                          equivalent bucket exists under src/lib/engine/.
///   action                 same store.
///   ingest                 needs the vendor hook-event adapters.
///   run start / run end    write `workflow_runs`. NOTE this is a
///                          DIFFERENT table from the `runs` table
///                          `planar.engine.runs.lifecycle` (which IS
///                          ported) operates on — that bucket's `start`
///                          mints a `run_uid` into `runs`, while
///                          `planar-agent run start` inserts a caller-
///                          supplied `run_identifier` into `workflow_runs`.
///                          Reusing the ported bucket here would write the
///                          wrong table; verified by reading
///                          zig/src/cmd/planar-agent/handlers/run/start.zig's
///                          literal INSERT.
///   dispatch / context     need the `routing_dispatch_*` and run-scoped
///                          context tables.
///
/// Omitting an unported child is the rule this tree inherits (task 6106:
/// `planar workflow --help` lists two commands where the oracle lists
/// three). The consequence, stated plainly because it is visible in
/// output: `planar-agent --help` here lists two verbs where the oracle
/// lists eighteen, and that page is therefore NOT oracle-comparable. LEAF
/// pages ARE, and are pinned byte-for-byte in `tree.t.cpp`.
///
/// The alternative — registering all eighteen and binding the unported
/// ones to a `not_implemented` stub so the root help page matched — was
/// considered and rejected: a registered verb that exits 64 looks like a
/// working verb to a script, and it would make the capability-boundary
/// tests assert a surface the binary cannot actually deliver.
module;

export module planar.cmd.planar_agent.tree;

import std;
import planar.cli;

namespace planar::cmd::agent {

/// @brief Build the `planar-agent` root command tree.
/// @return The root node.
export auto root_command() -> cli::cmd;

/// @brief The planning-entity verbs that must NEVER appear in this
/// binary's tree, at any depth — the forbidden half of the capability
/// boundary (zig/integration_tests/capability_boundary_test.zig's
/// `assertContainsNone` list for `planar-agent`, transcribed).
///
/// Exported rather than hidden in the test file so the list is part of the
/// binary's documented contract and a reader of this module sees what the
/// boundary actually excludes.
/// @return The forbidden verb names.
export auto forbidden_verbs() -> std::vector<std::string_view>;

} // namespace planar::cmd::agent
