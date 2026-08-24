/// @file surface.cppm
/// @brief `planar.cmd.planar_watch.surface` — the `planar-watch` binary's
/// FULL declared command surface, generated from the Zig oracle's own
/// catalog (plan 996, task 6065).
///
/// The same mechanism `planar.cmd.planar.surface` documents in full. The
/// oracle exposes 13 leaves; `tree.cpp` declares nine by hand with their
/// handlers. The four that remain — `feed`, `sync-events` and both `run`
/// verbs — are declared from generated data and registered as
/// `not_implemented` (exit 64).
///
/// ## One divergence this task did NOT close
///
/// A bare `planar-watch` routes to `feed` in the oracle. Here it renders
/// the root help page and exits 0, which was already true before `feed`
/// was declared at all and is documented at that site in
/// `planar.cmd.planar_watch.dispatch`. Closing it needs this binary's
/// `run` to grow the dual-node rule `planar`'s already has, plus a root
/// entry in a table whose reachability gate is leaf-only — a change to
/// dispatch semantics rather than to the declared surface, and out of this
/// task's scope. It is recorded here so it is not mistaken for something
/// the full-surface declaration fixed.
module;

export module planar.cmd.planar_watch.surface;

import std;
import planar.cliapp.surface;

namespace planar::cmd::watch {

/// @brief Every command node the oracle declares, ordered parent-before-child.
/// @return The full surface description.
export auto surface_nodes() -> std::vector<cliapp::node_spec>;

/// @brief The oracle's one-line `summary` for each command path.
///
/// Twelve of `planar-watch`'s fifteen nodes have a summary that differs
/// from their long description — proportionally the densest of the three
/// binaries, and ten of the twelve are leaves.
/// @return `(full command path, summary)` pairs.
export auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const>;

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED.
/// @return The inventory, sorted.
export auto unported_paths() -> std::span<std::string_view const>;

} // namespace planar::cmd::watch
