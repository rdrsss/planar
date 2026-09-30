/// @file surface.cppm
/// @brief `planar.cmd.planar_watch.surface` — the `planar-watch` binary's
/// summary-vs-description divergence table and its (currently empty)
/// unported-leaf inventory.
///
/// ## Before task 6613 (M11.1)
///
/// This module used to also export `surface_nodes()`: a full generated
/// `node_spec` table for all fourteen of the oracle's declared paths,
/// applied over `tree.cpp`'s hand-written tree via `cliapp::apply_surface`.
/// Nine of those fourteen nodes were ALSO hand-declared in `tree.cpp`, and
/// `apply_surface` silently skips a node already present under its parent
/// (`src/lib/cliapp/surface.cpp`'s `apply_surface`, the find-or-create
/// precedence rule) — so those nine `node_spec` entries were dead data,
/// shadowed by `tree.cpp`'s own declarations, which is what actually
/// shipped. Only the remaining five (`feed`, `run`, `sync-events`, `run
/// list`, `run show`) were ever live.
///
/// Task 6613 folded all fourteen into `tree.cpp` directly — the
/// `planar-ext` shape: one declaration site per node, no precedence rule to
/// reason about. `surface_nodes()` and its generated implementation file
/// (`surface.cpp`) are gone; a byte-diff of the two shadowed-node
/// descriptions (`tree.cpp`'s hand-written text vs. the deleted
/// `surface.cpp`'s generated text) confirmed the nine agreed exactly before
/// the fold, so nothing about the shipped surface moved.
///
/// ## What is left, and why it still earns its own module
///
/// `surface_summaries()` is a real, load-bearing divergence: twelve of
/// `planar-watch`'s fifteen `schema`-catalog nodes report a one-line
/// `summary` that differs from their long `description` (`CLI::App` itself
/// carries only one description string — see `tree.cpp`'s header — so the
/// summary has to come from somewhere else). `handlers::schema` reads this
/// table to fill that field; folding it into `tree.cpp` would mean baking
/// per-node summary strings into `root_app()` itself, which is a second,
/// parallel concern from "what does this binary's argv shape look like"
/// and earns staying separate.
///
/// `unported_paths()` is the (now empty) declared-but-unreachable-handler
/// inventory `dispatch.cpp`'s `handlers()` still consults; it is pinned
/// empty rather than deleted so a future regression there is still caught
/// (see `capability.t.cpp`'s "every planar-watch verb is either
/// implemented or refuses at 64").
///
/// Both functions are defined directly in this interface unit rather than a
/// separate `surface.cpp` (the same single-file-module shape
/// `planar.cmd.planar_watch.exit` uses) — there is no longer enough
/// implementation here to earn a split translation unit.
module;

export module planar.cmd.planar_watch.surface;

import std;

namespace planar::cmd::watch {

/// @brief The oracle's one-line `summary` for each command path.
///
/// Twelve of `planar-watch`'s fifteen nodes have a summary that differs
/// from their long description — proportionally the densest of the three
/// binaries, and ten of the twelve are leaves.
/// @return `(full command path, summary)` pairs.
export auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const> {
  static constexpr std::pair<std::string_view, std::string_view> k_summaries[] = {
      {"planar-watch", "Read-only viewer for live agent activity (feed / ps / claims / actions / plans / log / tree / run)."},
      {"planar-watch feed", "Cross-cutting activity feed across all vendors (default verb)."},
      {"planar-watch ps", "Snapshot of active (and stale) agent claims."},
      {"planar-watch claims", "List claims in the agent_work_claims ledger (filterable by status)."},
      {"planar-watch actions", "List agent_actions rows with optional filters."},
      {"planar-watch plans", "List plans with in-flight agent work."},
      {"planar-watch log", "Per-entity / per-claim history (union of agent actions and claim transitions)."},
      {"planar-watch tree", "Render the orchestrator → sub-agent action forest."},
      {"planar-watch run", "Observe workflow runs and their context records."},
      {"planar-watch run list", "List workflow runs (filterable by plan, status, and source arm)."},
      {"planar-watch run show", "Show one workflow run plus its context_records grouped by stage."},
      {"planar-watch sync-events", "List sync_events rows with optional filters (read-only)."},
      {"planar-watch queue", "List the host build and test queue: running and waiting entries, marking any that are not live."},
      {"planar-watch version", "Print the planar-watch version, commit, and C++ toolchain."},
      {"planar-watch completion", "Generate the autocompletion script for the specified shell."},
      {"planar-watch schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
  };
  return k_summaries;
}

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED.
/// @return The inventory, sorted. Empty since task 6448 (`run list`, `run
/// show`, `sync-events` all landed real handlers); kept as a named,
/// scanner-recognized initializer rather than deleted — see this module's
/// header.
export auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {std::string_view{}};
  return std::span<std::string_view const>{k_unported}.first(0);
}

} // namespace planar::cmd::watch
