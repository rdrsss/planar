/// @file tree.cppm
/// @brief `planar.cmd.planar_watch.tree` — the `planar-watch` binary's root
/// command tree (plan 996, task 6107).
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
/// half by `tree.t.cpp`, the handle half by `context.t.cpp` executing a
/// real `insert` through the handle and requiring it to fail.
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
/// ## What this tree deliberately is NOT (yet)
///
/// The oracle registers twelve verbs. This tree registers THREE —
/// `version`, `schema`, `completion` — and, as with `planar-agent`, the gap
/// is a porting gap, not a capability statement.
///
/// EVERY remaining verb is blocked one layer down, and on the SAME bucket:
/// `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree` and `run` all
/// read through zig `engine.runtime.agentactivity` (its `store`, `types`
/// and `json` submodules — 5,702 lines across seven files), which this tree
/// has never ported. That is the identical blocker task 6106 found under
/// `planar dashboard`; it is one bucket gating nine leaves across two
/// binaries, which is what makes it a milestone of its own rather than
/// something to chip at.
///
/// `sync-events` is the one exception worth naming precisely, because it
/// LOOKS portable and is not quite: its own query is over `sync_events`
/// joined to `external_links` (both reachable from the ported
/// `engine_external` bucket), but it reaches into `ps` for the shared
/// `generated_at` emitter and interval parsing, and its `--follow` arm
/// needs the SIGINT/poll loop `follow.zig` owns. It is the cheapest next
/// read verb, not a free one.
///
/// Two consequences visible in output, stated rather than hidden:
///
///   - `planar-watch --help` lists three verbs where the oracle lists
///     twelve, so the ROOT page is not oracle-comparable. Leaf pages are,
///     and are pinned byte-for-byte in `tree.t.cpp`.
///   - The oracle's DEFAULT VERB on a bare invocation is `feed`. `feed` is
///     unported, so a bare `planar-watch` here renders the root help page
///     instead. That is a real divergence, not a rendering accident, and
///     it disappears when `feed` lands.
module;

export module planar.cmd.planar_watch.tree;

import std;
import planar.cli;

namespace planar::cmd::watch {

/// @brief Build the `planar-watch` root command tree.
/// @return The root node.
export auto root_command() -> cli::cmd;

/// @brief The verbs that must NEVER appear in this binary's tree, at any
/// depth: every `planar-agent` write verb plus every `planar`
/// planning-entity verb (zig/integration_tests/capability_boundary_test.zig's
/// two `assertContainsNone` lists for `planar-watch`, transcribed and
/// concatenated).
/// @return The forbidden verb names.
export auto forbidden_verbs() -> std::vector<std::string_view>;

} // namespace planar::cmd::watch
