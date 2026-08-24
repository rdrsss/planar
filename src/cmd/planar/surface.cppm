/// @file surface.cppm
/// @brief `planar.cmd.planar.surface` — the `planar` binary's FULL declared
/// command surface, generated from the Zig oracle's own catalog (plan 996,
/// task 6065).
///
/// ## Why generated, and why data
///
/// The oracle exposes 223 leaves under 47 top-level verbs, carrying 654
/// locally-declared flags and 179 positionals. Seven top-level verbs were
/// hand-transcribed into `tree.cpp` across tasks 6105/6106/6040/6041, with
/// a paragraph of rationale each — the right shape for a verb whose
/// HANDLER is landing in the same cycle. It is the wrong shape for the
/// remaining ~190, which land no behaviour at all: a hand-written
/// declaration of a verb nobody implemented is 3,600 lines of transcription
/// that no reviewer can check against anything except the oracle, which is
/// exactly what a generator can check mechanically and continuously.
///
/// So `scripts/gen-cli-surface.py` reads `planar schema` from
/// `zig/zig-out/bin/planar` and emits `surface.cpp`. `tree.cpp` stays the
/// authoritative hand-written declaration for every PORTED verb —
/// `planar.cliapp.surface::apply_surface` skips any node that already
/// exists — and the generated data fills only the gaps.
///
/// ## What a declared-but-unported verb DOES
///
/// It refuses, loudly, at exit 64. `unported_paths()` is the explicit
/// inventory, and `planar.cmd.planar.dispatch` registers a
/// `not_implemented` handler for every entry. Two distinct hazards make
/// that registration load-bearing rather than decorative:
///
///   * a LEAF left out of the table would still exit 64 via `run`'s
///     table-miss arm — but it would also trip the "every leaf in the tree
///     has a handler" gate, which is the thing that keeps the inventory
///     honest as verbs get ported;
///   * a DUAL node (subcommands AND its own handler in the oracle) left
///     out of the table falls to the HELP path and exits 0 — a silent
///     success. Measured by invoking every one of the oracle's 38 group
///     nodes against a scratch arena: exactly three are dual — `resume`
///     (exit 1 here, scope error), `handoff` (exit 2, no active session)
///     and `health` (exit 0 WITH a report). `resume` was already
///     registered for this reason in task 6040; `health` is registered in
///     this task for the same reason. Every other group renders help at
///     exit 0, which is what an unregistered group does, so those are
///     correct unregistered.
module;

export module planar.cmd.planar.surface;

import std;
import planar.cliapp.surface;

namespace planar::cmd {

/// @brief Every command node the oracle declares, ordered parent-before-child.
/// @return The full surface description.
export auto surface_nodes() -> std::vector<cliapp::node_spec>;

/// @brief The oracle's one-line `summary` for each command path.
///
/// `CLI::App` holds a single description string, so the short summary has
/// nowhere to live on the tree and is handed to
/// `planar.cliapp.schema::schema_json` as data instead. 57 `planar` nodes
/// have a summary that differs from their long description, 28 of them
/// leaves.
/// @return `(full command path, summary)` pairs.
export auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const>;

/// @brief The root-relative path keys that are DECLARED but not
/// IMPLEMENTED — every unported leaf, plus the unported dual nodes.
/// @return The inventory, sorted.
export auto unported_paths() -> std::span<std::string_view const>;

} // namespace planar::cmd
