/// @file tree.cppm
/// @brief `planar.cmd.planar.tree` — the `planar` binary's root command
/// tree, built directly as a `CLI::App` (plan 996, tasks 6105 and 6123).
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
/// the ported children. `planar --help` shows six verbs, not forty-seven;
/// `planar workflow --help` shows `list` and `show` but not `run`
/// (deferred with its process-spawn dependency —
/// `src/lib/engine/workflows/CMakeLists.txt` records why), and `planar
/// workspace --help` shows `doctor` alone where the oracle shows four.
/// Those pages are therefore NOT oracle-comparable and no test claims they
/// are.
///
/// LEAF help pages are a different matter and ARE pinned byte-for-byte:
/// a leaf's page is derived entirely from its own node — name, desc,
/// long_desc, flags, positionals — so a leaf that is ported at all is
/// ported completely, and its `--help` output either matches the oracle
/// exactly or the node was transcribed wrong. See `tree.t.cpp`.
///
/// ## Node metadata is transcribed, not invented
///
/// Every description / flag name / default below is transcribed from the
/// Zig node definition and then CHECKED against the oracle — as of task
/// 6123 against its `schema` CATALOG rather than its rendered `--help`
/// bytes, since CLI11 renders help now and the pages no longer compare.
/// The catalog is the better check anyway: it states flags, required-ness
/// and positionals directly instead of through a layout. See
/// `src/cmd/planar/parity.t.cpp`'s "every ported command declares what the
/// oracle declares".
///
/// NOTE `CLI::App` carries ONE description string where `cli::cmd` carried
/// a one-line `desc` and a multi-line `long_desc` separately. Where a node
/// had both, the longer operator-facing prose is what survives — a named
/// loss of the swap, recorded in `planar.cliapp.schema`'s header.
///
/// CLI11 CAN bind a callback per subcommand (`App::callback`), and this
/// tree deliberately does not use it: a handler needs this binary's
/// `context` and returns its `domain_error`, neither of which fits a
/// `std::function<void()>` captured at tree-build time. Binding a leaf to
/// code is layer 3's business and lives in `planar.cmd.planar.dispatch`'s
/// path-keyed table instead — see that module's header.
module;

export module planar.cmd.planar.tree;

import std;
import cli11;

namespace planar::cmd {

/// @brief Build the `planar` root command tree.
///
/// Returns a `unique_ptr` rather than a value: `CLI::App` holds raw
/// parent/child back-pointers, so a moved or copied tree would leave every
/// subcommand pointing at a dead parent. Pointer-stable ownership is not a
/// style choice here.
///
/// Task 6123 replaced the hand-rolled `cli::cmd` data tree with a
/// `CLI::App` built here; CLI11 now owns tokenization, value coercion,
/// subcommand resolution, required/choice enforcement and help rendering.
/// The four binaries deliberately do NOT share a tree builder — D18 makes a
/// `cmd_* -> cmd_*` edge a configure-time FATAL, and the binaries genuinely
/// differ. What they share is layer-1 `planar.cliapp`, which only ever
/// DESCRIBES a tree it is handed.
/// @return The root app, owning every subcommand beneath it.
export auto root_app() -> std::unique_ptr<CLI::App>;

} // namespace planar::cmd
