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
/// ## Complete declared surface
///
/// The oracle's eighteen top-level verbs are all declared and wired here.
/// `ingest` normalizes the Claude and Copilot hook envelopes at layer 3,
/// then composes the runtime session/action primitives atomically.  `run`,
/// `dispatch`, and `context` land through their corresponding runtime and
/// routing stores.  Task 6614 folded the nine verbs that used to be
/// declared from a generated `node_spec` table (`ingest`, both `run`
/// leaves, both `dispatch` leaves, all four `context` leaves) directly
/// into this function — one declaration site per node, matching the
/// `planar-ext` shape. This tree is now the single source of truth for
/// the schema catalog; `planar.cmd.planar_agent.surface` survives only for
/// its `unported_paths()` inventory (see that module's header).
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
