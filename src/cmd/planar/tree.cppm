/// @file tree.cppm
/// @brief `planar.cmd.planar.tree` — the `planar` binary's root command
/// tree (plan 996, task 6105).
///
/// Port target: the `root` constant in zig/src/cmd/planar/main.zig, plus
/// the `pub const verb: cli.Cmd` each `handlers/<verb>.zig` /
/// `handlers/<verb>/cmd.zig` contributes to it.
///
/// ## What this tree deliberately is NOT
///
/// It is not the full 47-verb tree. This task stands up the layer-3
/// ARCHITECTURE and proves it with a small, honestly chosen verb subset;
/// porting the remaining verbs is the milestones that follow. The
/// consequence to be clear about, because it is visible in output: any
/// help page for a node whose children are only PARTLY ported lists only
/// the ported children. `planar --help` shows four verbs, not
/// forty-seven; `planar workflow --help` shows `list` and `show` but not
/// `run` (deferred with its process-spawn dependency —
/// `src/lib/engine/workflows/CMakeLists.txt` records why). Those pages are
/// therefore NOT oracle-comparable and no test claims they are.
///
/// LEAF help pages are a different matter and ARE pinned byte-for-byte:
/// a leaf's page is derived entirely from its own node — name, desc,
/// long_desc, flags, positionals — so a leaf that is ported at all is
/// ported completely, and its `--help` output either matches the oracle
/// exactly or the node was transcribed wrong. See `tree.t.cpp`.
///
/// ## Node metadata is transcribed, not invented
///
/// Every `desc` / `long_desc` / flag name / default below is transcribed
/// from the Zig node definition and then CHECKED against the oracle's
/// rendered `--help` bytes, which is the direction that actually catches a
/// mistake: a wrong default or a dropped flag changes the rendered page.
///
/// The `run` handler field etcli's `Cmd` carries has no counterpart here.
/// `planar.cli.cmd` is a LAYER-1 type and deliberately models no handler
/// (its own header says so); binding a leaf to code is layer 3's business
/// and lives in `planar.cmd.planar.dispatch`'s table instead.
module;

export module planar.cmd.planar.tree;

import std;
import planar.cli;

namespace planar::cmd {

/// @brief Build the `planar` root command tree.
///
/// Returned by value rather than exposed as a namespace-scope constant:
/// `cli::cmd` is ordinary runtime data (see `planar.cli.cmd`'s header —
/// nothing here needs compile-time evaluation to pay for itself), and a
/// function keeps the tree out of static-initialisation order entirely.
/// Callers that need it more than once should build it once and pass it
/// around; `dispatch::run` does exactly that.
/// @return The root node.
export auto root_command() -> cli::cmd;

} // namespace planar::cmd
