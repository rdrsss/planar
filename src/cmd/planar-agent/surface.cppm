/// @file surface.cppm
/// @brief `planar.cmd.planar_agent.surface` — the `planar-agent` binary's
/// FULL declared command surface, generated from the Zig oracle's own
/// catalog (plan 996, task 6065).
///
/// The same mechanism `planar.cmd.planar.surface` documents in full, at a
/// much smaller scale: the oracle exposes 24 leaves, 15 of which
/// `tree.cpp` declares by hand alongside their handlers. The nine that
/// remain — the four `context` verbs, both `dispatch` verbs, both `run`
/// verbs and `ingest` — are declared from generated data and registered as
/// `not_implemented` (exit 64).
///
/// No node in this binary's oracle tree is DUAL: every group (`action`,
/// `run`, `dispatch`, `context`) and a bare `planar-agent` render help at
/// exit 0, measured by invoking each against a scratch arena. So the
/// unported inventory here is leaves only.
module;

export module planar.cmd.planar_agent.surface;

import std;
import planar.cliapp.surface;

namespace planar::cmd::agent {

/// @brief Every command node the oracle declares, ordered parent-before-child.
/// @return The full surface description.
export auto surface_nodes() -> std::vector<cliapp::node_spec>;

/// @brief The oracle's one-line `summary` for each command path.
///
/// Zero `planar-agent` nodes have a summary that differs from their long
/// description — the table is supplied anyway so the emitter is driven the
/// same way in all three binaries rather than only where it currently
/// changes bytes.
/// @return `(full command path, summary)` pairs.
export auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const>;

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED.
/// @return The inventory, sorted.
export auto unported_paths() -> std::span<std::string_view const>;

} // namespace planar::cmd::agent
