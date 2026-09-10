/// @file surface.cppm
/// @brief `planar.cmd.planar_agent.surface` — the `planar-agent` binary's
/// (currently empty) unported-leaf inventory.
///
/// ## Before task 6614 (M11.2)
///
/// This module used to also export `surface_nodes()` (a full generated
/// `node_spec` table for all twenty-eight of the oracle's declared paths,
/// applied over `tree.cpp`'s hand-written tree via `cliapp::apply_surface`)
/// and `surface_summaries()`. Sixteen of the twenty-eight `node_spec`
/// entries were ALSO hand-declared in `tree.cpp`, and `apply_surface`
/// silently skips a node already present under its parent
/// (`src/lib/cliapp/surface.cpp`'s `apply_surface`, the find-or-create
/// precedence rule) — so those sixteen `node_spec` entries were dead data,
/// shadowed by `tree.cpp`'s own declarations, which is what actually
/// shipped. Only the remaining twelve (`ingest`, the `run`/`dispatch`/
/// `context` groups, and their nine leaves) were ever live.
///
/// Task 6614 folded all twenty-eight into `tree.cpp` directly — the
/// `planar-ext` shape: one declaration site per node, no precedence rule to
/// reason about. `surface_nodes()` and its generated implementation file
/// (`surface.cpp`) are gone; a byte-diff of the sixteen shadowed-node
/// descriptions and flag tables (`tree.cpp`'s hand-written text vs. the
/// deleted `surface.cpp`'s generated text) confirmed they agreed exactly
/// before the fold, so nothing about the shipped surface moved.
///
/// `surface_summaries()` is also gone, and — unlike `planar-watch`
/// (task 6613) — that is NOT a relaxed target here. `planar.cliapp.schema`'s
/// own header records the measurement: across the three oracle catalogs,
/// 0 of `planar-agent`'s nodes emit a `summary` that differs from their
/// `description` (57 do on `planar`, 12 on `planar-watch`). `handlers/
/// schema.cpp` now calls the single-argument `schema_json(root)` overload
/// — same reasoning `planar-ext` already used ("this tree carries no
/// summary table, nothing to diverge from") — because there is genuinely
/// nothing for `planar-agent` to diverge from either; it just took a real
/// measurement, not an oracle-less binary, to prove it. Folding a table
/// that never disagreed with `description` changes nothing in the pinned
/// catalog, which decision 1068 requires and `make surface-check` verifies
/// byte-for-byte.
///
/// ## What is left, and why it still earns its own module
///
/// `unported_paths()` is the (now empty) declared-but-unreachable-handler
/// inventory `dispatch.cpp`'s `handlers()` still consults; it is pinned
/// empty rather than deleted so a future regression there is still caught
/// (see `capability.t.cpp`'s "every planar-agent verb is either
/// implemented or refuses at 64").
///
/// Defined directly in this interface unit rather than a separate
/// `surface.cpp` (the same single-file-module shape
/// `planar.cmd.planar_agent.exit` uses) — there is no longer enough
/// implementation here to earn a split translation unit.
module;

export module planar.cmd.planar_agent.surface;

import std;

namespace planar::cmd::agent {

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED.
/// @return The inventory, sorted. Empty — every one of `planar-agent`'s
/// twenty-eight declared paths has a real handler in `dispatch.cpp`; kept
/// as a named, scanner-recognized initializer rather than deleted — see
/// this module's header.
export auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {std::string_view{}};
  return std::span<std::string_view const>{k_unported}.first(0);
}

} // namespace planar::cmd::agent
