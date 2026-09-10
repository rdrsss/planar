/// @file tree.cppm
/// @brief `planar.cmd.planar_ext.tree` — the `planar-ext` binary's root
/// command tree, built directly as a `CLI::App` (plan 996, task 6418).
///
/// ## THIS TASK LANDS NO VERBS
///
/// Task 6418 is the skeleton step of task 6412's four-task extraction
/// (decisions 995-1000): stand up the binary, its schema catalog, and its
/// write-capability boundary, with `planar` still owning every `ext`/
/// `sync` verb it owns today. So this tree declares exactly TWO leaves —
/// `version` and `schema` — the same meta-verbs every sibling binary
/// carries. Tasks 6419-6421 add the real `ext`/`sync` surface here and
/// remove it from `planar`'s tree in the same cycle, so the operator
/// contract never has a window where a verb exists on both binaries or
/// neither.
///
/// ## Why this is a `CLI::App`, and why it is not shared
///
/// Same reasoning as the other three binaries (see
/// `planar.cmd.planar_agent.tree`'s header): D18 makes a `cmd_* -> cmd_*`
/// edge a configure-time FATAL, and each binary owns its own tree because
/// the tree — together with `planar.cmd.planar_ext.context`'s
/// write-capability policy — IS this binary's capability boundary.
///
/// `root_app` returns a `unique_ptr` rather than a value for the same
/// reason as its siblings: `CLI::App` holds raw parent/child back-pointers,
/// so a moved or copied tree would leave a subcommand pointing at a dead
/// parent.
module;

export module planar.cmd.planar_ext.tree;

import std;
import cli11;

namespace planar::cmd::ext {

/// @brief Build the `planar-ext` root command tree.
/// @return The root app, owning every subcommand beneath it.
export auto root_app() -> std::unique_ptr<CLI::App>;

} // namespace planar::cmd::ext
