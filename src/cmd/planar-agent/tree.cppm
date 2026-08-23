/// @file tree.cppm
/// @brief `planar.cmd.planar_agent.tree` — the `planar-agent` binary's root
/// command tree, built directly as a `CLI::App` (plan 996, tasks 6107
/// and 6123).
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
/// `capability.t.cpp`: none of `plan`, `task`, `decision`, `question`,
/// `scenario`, `artifact`, `annotate`, `init`, `workbench`, `doc`, `spec`,
/// `templates`, `ext`, `sync`, `promote`, `demote`, `capture`,
/// `dashboard`, `tree`, `health` may appear here, at any depth, ever.
///
/// ## What this tree deliberately is NOT (yet)
///
/// The oracle registers eighteen verbs. This tree registers FOURTEEN —
/// task 6038 landed the whole claim ritual (`pull`, `peek`, `claim`,
/// `heartbeat`, `claim-associate`, `complete`, `fail`, `release`, `block`,
/// `action start`/`end`, `reconcile`, `abort`) on top of `version` and
/// `schema`. The remaining FOUR are blocked ONE LAYER DOWN, on layer-2
/// buckets this tree has never ported:
///
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
///                          literal INSERT. (`reconcile` DOES sweep
///                          `workflow_runs`, so the table is reachable —
///                          what is missing is the run LIFECYCLE surface.)
///   dispatch / context     need the `routing_dispatch_*` and run-scoped
///                          context tables. `routing_dispatch_previews`
///                          alone carries twenty-odd bound columns and a
///                          single-use trigger; it is its own cycle.
///
/// Omitting an unported child is the rule this tree inherits (task 6106:
/// `planar workflow --help` lists two commands where the oracle lists
/// three). The consequence, stated plainly because it is visible in
/// output: `planar-agent --help` here lists fourteen verbs where the
/// oracle lists eighteen, and that page is therefore still NOT
/// oracle-comparable. LEAF pages ARE, and all fourteen are diffed
/// byte-for-byte against the live oracle in `parity.t.cpp`.
///
/// The alternative — registering all eighteen and binding the unported
/// ones to a `not_implemented` stub so the root help page matched — was
/// considered and rejected: a registered verb that exits 64 looks like a
/// working verb to a script, and it would make the capability-boundary
/// tests assert a surface the binary cannot actually deliver.
module;

export module planar.cmd.planar_agent.tree;

import std;
import cli11;

namespace planar::cmd::agent {

/// @brief Build the `planar-agent` root command tree.
///
/// Returns a `unique_ptr` rather than a value: `CLI::App` holds raw
/// parent/child back-pointers, so a moved or copied tree would leave every
/// subcommand pointing at a dead parent. Pointer-stable ownership is not a
/// style choice here.
///
/// Task 6123 replaced the hand-rolled `cli::cmd` tree with a `CLI::App`
/// built here. The four binaries deliberately do NOT share a tree builder
/// — D18 makes a `cmd_* -> cmd_*` edge a configure-time FATAL, and this
/// binary's verb set IS the capability boundary. What they share is
/// layer-1 `planar.cliapp`, which only ever DESCRIBES a tree it is handed.
/// @return The root app, owning every subcommand beneath it.
export auto root_app() -> std::unique_ptr<CLI::App>;

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
