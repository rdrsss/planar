/// @file surface.cppm
/// @brief `planar.cliapp.surface` — declare a whole command surface onto a
/// built `CLI::App` from a flat, data-only description of it (plan 996,
/// task 6065).
///
/// ## The problem this solves
///
/// `zig/tools/cli_usage_lint` is a live gate (`make cli-usage-check`). It
/// reads `<bin> schema` and flags any `--flag` an authored doc references
/// on a command the binary exposes. Its blind spot, measured in
/// `schema.t.cpp`'s `[lint-parity]` scope case, is that a command path it
/// cannot resolve is SKIPPED rather than flagged — so a catalog covering
/// seven of forty-seven `planar` verbs makes the gate quietly vacuous over
/// the other forty. Closing that needs the FULL surface declared: 223
/// leaves on `planar`, 24 on `planar-agent`, 13 on `planar-watch`.
///
/// Hand-transcribing ~870 flags and ~180 positionals into `tree.cpp` would
/// be both enormous and un-reviewable. So the surface that is not yet
/// IMPLEMENTED is declared from data generated directly out of the
/// oracle's own catalog (`scripts/gen-cli-surface.py` → each binary's
/// `surface.cpp`), and this module applies that data to a tree.
///
/// ## Declaring is not implementing, and the difference is loud
///
/// A declared node that dispatches to nothing is WORSE than an absent one
/// if it exits 0. Two rules keep that from happening, and neither lives
/// here — they live in each binary's dispatch table, which this module
/// deliberately does not touch:
///
///   * a declared LEAF with no handler falls to `run`'s table-miss arm and
///     exits 64 (`not_implemented`) with a message on stderr;
///   * a declared DUAL node (subcommands AND its own handler in the
///     oracle — measured to be exactly `planar resume`, `planar handoff`
///     and `planar health`) must be REGISTERED with an explicit
///     `not_implemented` handler, because an unregistered node with
///     children falls to the help path and exits 0 — a silent success.
///
/// Each binary's generated `unported_paths()` is the explicit inventory of
/// both, so the "every leaf has a handler" registration gate keeps
/// discriminating: a node in neither the real table nor that inventory
/// still fails it.
///
/// ## Find-or-create, never overwrite
///
/// `apply_surface` SKIPS any node that already exists. The hand-written
/// `tree.cpp` declarations stay authoritative for every ported verb —
/// including the several places where they diverge from a literal reading
/// of the oracle on purpose (a `<link-id>` declared as a string so the
/// refusal comes from the handler, `--filter-mode` kept on `archive` even
/// though the engine ignores it). Only the gaps are filled.
module;

export module planar.cliapp.surface;

import std;
import cli11;

namespace planar::cliapp {

/// @brief One flag's declaration, as the oracle's catalog reports it.
export struct flag_spec {
  std::string_view name;                  ///< Canonical long name, `--` included.
  std::string_view kind;                  ///< `bool`, `int` or `string`.
  bool             required      = false; ///< Whether the oracle marks it required.
  bool             list          = false; ///< Whether it is repeatable.
  std::string_view default_value = {};    ///< The declared default, or empty for none.
  std::string_view description   = {};    ///< The help line.
};

/// @brief One positional's declaration.
///
/// No `kind`: every positional across the three unported surfaces is
/// `string` in the oracle's catalog (the two `int` ones, `planar-agent
/// pull`/`peek`'s `<plan-id>`, are already ported by hand). Declaring a
/// kind field with one possible value would invite a future generator to
/// attach an integer validator and move a handler-level refusal a layer
/// earlier — the divergence `planar unlink`'s header warns about.
export struct positional_spec {
  std::string_view name;                ///< The positional's name.
  bool             required    = false; ///< Whether the oracle marks it required.
  std::string_view description = {};    ///< The help line.
};

/// @brief One command node's declaration.
export struct node_spec {
  std::span<std::string_view const> path;          ///< Root-relative subcommand path.
  std::string_view                  description;   ///< The oracle's long description.
  std::span<flag_spec const>        flags;         ///< Locally declared flags.
  std::span<positional_spec const>  positionals;   ///< Positionals, in declaration order.
  bool                              group = false; ///< Whether the node has children.
};

/// @brief Declare every node in `nodes` that `root` does not already carry.
///
/// `nodes` must be ordered parent-before-child; the generator sorts by
/// path depth to guarantee it. A spec whose PARENT cannot be resolved is
/// reported rather than silently dropped — that can only happen if the
/// ordering invariant breaks.
/// @param root The tree to extend, already carrying its hand-written nodes.
/// @param nodes The full surface description.
/// @return The root-relative path keys of the nodes actually CREATED, in
/// creation order, plus (prefixed with `!`) any spec whose parent was
/// unresolvable.
export auto apply_surface(CLI::App& root, std::span<node_spec const> nodes) -> std::vector<std::string>;

} // namespace planar::cliapp
