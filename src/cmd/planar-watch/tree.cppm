/// @file tree.cppm
/// @brief `planar.cmd.planar_watch.tree` — the `planar-watch` binary's root
/// command tree, built directly as a `CLI::App` (plan 996, tasks 6107 and
/// 6123).
///
/// Port target: the `root` constant in zig/src/cmd/planar-watch/main.zig.
///
/// ## THE VERB SET IS THE FIRST LINE OF THE READ-ONLY GUARANTEE
///
/// zig/integration_tests/capability_boundary_test.zig puts it precisely:
/// "planar-watch is read-only at two levels: (1) its verb set has zero
/// write verbs; (2) its DB handle is opened with SQLITE_OPEN_READONLY so
/// the driver itself rejects write SQL." This function is level (1).
/// `planar.cmd.planar_watch.context::ensure_db` is level (2). Neither
/// subsumes the other, and both are tested independently — the verb-set
/// half by `capability.t.cpp`, the handle half by `context.t.cpp`
/// executing a real `insert` through the handle and requiring it to fail.
///
/// The forbidden set is BOTH of the other binaries' write surfaces: every
/// `planar-agent` write verb (`pull`, `claim`, `heartbeat`, `complete`,
/// `fail`, `release`, `block`, `action`, `ingest`, `reconcile`, `abort`,
/// `peek`) AND every `planar` planning-entity verb (`plan`, `task`,
/// `decision`, `question`, `scenario`, `artifact`, `annotate`, `init`,
/// `workbench`, `doc`, `spec`, `templates`, `ext`, `sync`, `promote`,
/// `demote`, `capture`). A watcher configured with only `planar-watch` on
/// `PATH` cannot touch anything at all, and that sentence is true only
/// because this function keeps it true.
///
/// ## Task 6123: this is a `CLI::App`, and each binary owns its own
///
/// The tree used to be `cli::cmd` data walked by a hand-rolled parser in
/// `src/lib/cli`. That library is gone; CLI11 owns tokenization, value
/// coercion, subcommand resolution and help rendering now. The four
/// binaries deliberately do NOT share a tree builder — D18 makes a
/// `cmd_* -> cmd_*` edge a configure-time FATAL, and this binary's tree is
/// the thing the capability boundary is ABOUT. What they share is layer-1
/// `planar.cliapp`, which only ever DESCRIBES a tree it is handed.
///
/// `root_app` returns a `unique_ptr` rather than a value: `CLI::App` holds
/// raw parent/child back-pointers, so a moved or copied tree would leave
/// every subcommand pointing at a dead parent. Pointer-stable ownership is
/// not a style choice here.
///
/// ## What this tree deliberately is NOT (yet)
///
/// The oracle registers twelve verbs. This tree registers THREE —
/// `version`, `schema`, `completion` — and the gap is a porting gap, not a
/// capability statement. `feed`, `ps`, `claims`, `actions`, `plans`,
/// `log`, `tree` and `run` all read through zig
/// `engine.runtime.agentactivity` (store/types/json — 5,702 lines across
/// seven files), which this tree has never ported; `sync-events` reaches
/// into `ps` for its shared `generated_at` emitter and needs `follow.zig`'s
/// SIGINT/poll loop for `--follow`.
///
/// Two consequences visible in output, stated rather than hidden: the ROOT
/// help page lists three verbs where the oracle lists twelve, and a BARE
/// `planar-watch` renders the root help page where the oracle runs its
/// default `feed` verb.
module;

export module planar.cmd.planar_watch.tree;

import std;
import cli11;

namespace planar::cmd::watch {

/// @brief Build the `planar-watch` root command tree.
/// @return The root app, owning every subcommand beneath it.
export auto root_app() -> std::unique_ptr<CLI::App>;

/// @brief The verbs that must NEVER appear in this binary's tree, at any
/// depth: every `planar-agent` write verb plus every `planar`
/// planning-entity verb (zig/integration_tests/capability_boundary_test.zig's
/// two `assertContainsNone` lists for `planar-watch`, transcribed and
/// concatenated).
/// @return The forbidden verb names.
export auto forbidden_verbs() -> std::vector<std::string_view>;

} // namespace planar::cmd::watch
