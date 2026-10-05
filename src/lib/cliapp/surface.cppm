/// @file surface.cppm
/// @brief `planar.cliapp.surface` — the parser-agreement primitives shared
/// by every binary's hand-written `CLI::App` tree: the `--no-X` negation
/// synthesis (`add_bool_flag`), the help formatter that keeps it out of
/// `--help` and lists each subcommand by its one-line summary
/// (`hide_negations_in_help`), and the position-independent-flag
/// argv reordering (`hoist_subcommands`).
///
/// ## What used to be here, and where it went
///
/// This module used to also export `node_spec` / `flag_spec` /
/// `positional_spec` and `apply_surface`: a flat, data-only description of
/// a command surface, generated from the Zig oracle's own catalog
/// (`scripts/gen-cli-surface.py` → each binary's `surface.cpp`), applied
/// with FIND-OR-CREATE semantics over each binary's hand-written
/// `tree.cpp` so a node already declared by hand was left untouched and
/// the surface's remaining gaps were filled in from the generated data.
///
/// That mechanism implemented a SILENT PRECEDENCE RULE: a node already
/// hand-declared in `tree.cpp` caused the corresponding `node_spec` to be
/// skipped entirely, so that spec's description, flags and positionals had
/// no effect and nothing said so at the declaration site — the rule is
/// what made a break-probe INERT during M11.0 (plan 1051, task 6616). Eight
/// waves (M11.1 through M11.3f) folded every generated declaration into
/// hand-written code beside its handler, and the last of them (task 6636)
/// deleted the final call site along with `surface.cpp`'s last
/// `node_spec` table. With no caller left in any binary, task 6616 removed
/// `apply_surface`, its structs, and `scripts/gen-cli-surface.py` (which
/// generated from the oracle's catalog and could not run once the oracle
/// was deleted at the M10 cutover, task 6045).
///
/// ## Declaring is not implementing, and the difference is loud
///
/// A declared node that dispatches to nothing is WORSE than an absent one
/// if it exits 0. Two rules keep that from happening:
///
///   * a declared LEAF with no handler falls to `run`'s table-miss arm and
///     exits 64 (`not_implemented`) with a message on stderr;
///   * a declared DUAL node (subcommands AND its own handler — exactly
///     `planar resume`, `planar handoff` and `planar health`) must be
///     REGISTERED with an explicit `not_implemented` handler, because an
///     unregistered node with children falls to the help path and exits 0
///     — a silent success.
///
/// Each binary's `unported_paths()` (in its own `surface.cppm`) is the
/// explicit inventory of both, so the "every leaf has a handler"
/// registration gate keeps discriminating: a node in neither the real
/// table nor that inventory still fails it.
module;

export module planar.cliapp.surface;

import std;
import cli11;

namespace planar::cliapp {

/// @brief The explicit marker for a flag or positional whose help text has
/// not been written yet.
///
/// Every flag-declaration helper requires a description. Passing this
/// empty view says "undocumented" at the call site, where a defaulted
/// parameter said nothing, and `grep -rn k_undocumented src` lists the
/// sweep that remains.
export constexpr std::string_view k_undocumented{};

/// @brief Declare a boolean flag together with the `--no-<name>` negation
/// the oracle's parser SYNTHESIZES for it (plan 996, task 6138).
///
/// ## The divergence this closes
///
/// etcli's `matchFlag` carries a `flagNegationMatches` arm
/// (`zig/vendor/etcli-zig/src/cli/parser.zig:771`) that accepts `--no-X`
/// for any flag whose `kind` is `bool` and whose `count` is false, and
/// assigns `false` — regardless of the flag's declared default. The
/// negation is NOT declared in the oracle's own `schema` catalog, so
/// `cli_usage_lint` has never been able to see it and CLI11, which
/// declares exactly what the surface declares, refused all 341 of them.
///
/// The semantics reproduced here, each derived by RUNNING the oracle
/// rather than read off its help:
///
///     planar task list --no-json          accepted, --json is false
///     planar task add … --no-editor       accepted for a default-TRUE flag too
///     planar task list --json --no-json   error: flag specified more than once
///     planar task list --no-json=true     error: unknown flag
///     planar task list --no-no-json       error: unknown flag
///     planar task list --no-scope x       error: unknown flag (non-bool: no negation)
///     planar-agent claim … --no-no-transition
///                                         accepted — the synthesis applies to a
///                                         flag ALREADY named `--no-*` too, so the
///                                         rule below has no name-shape exception
///
/// Two rows this form does not reproduce, both measured rather than
/// assumed, and neither reachable through the negation alone:
///
///   `--json --no-json`   TakeLast here, `DuplicateFlag` (exit 2) on the
///                        oracle. But `--json --json` diverges identically
///                        and predates this change entirely, so duplicate
///                        detection is a whole-parser gap, not a negation
///                        one. Confirmed by running both forms.
///   `--no-json=true`     Refused by the oracle (its negation arm is an
///                        EXACT token match, so `=value` never reaches it)
///                        and ACCEPTED here, because CLI11 resolves the
///                        `=value` against the negation's declared flag
///                        value. A deliberate, narrow WIDENING: the
///                        results string CLI11 records for `--no-json=true`
///                        is byte-identical to the one it records for a
///                        bare `--no-json`, so no validator downstream can
///                        tell them apart and refuse only the former.
///
/// The `check` below closes a third row that WAS diverging before this
/// task and is not about negation at all: `--json=bogus`.
///
/// ## Why the catalog does not grow
///
/// CLI11 files a `!`-prefixed name into `Option::fnames_` AND into
/// `lnames_`, so a naive emitter would report `--no-json` as an ALIAS of
/// `--json` and break the byte-level catalog parity `catalog_parity.hpp`
/// enforces. `planar.cliapp.schema::aliases_of` therefore skips any long
/// name that is also an `fname`. The catalog stays exactly as honest as
/// the oracle's — which is to say it still does not list the negations,
/// because the oracle's does not either.
///
/// ## Every bool flag, with no exceptions to carry
///
/// Measured across all three oracle catalogs: 341 bool flags, ZERO of
/// them `count`, ZERO of them carrying aliases, and ZERO commands where a
/// synthesized `--no-X` would collide with a separately declared `--no-X`
/// (the eight genuinely-declared `--no-*` flags all lack a `--X` sibling).
/// So the rule is unconditional and needs no exception list.
/// @param app The node to declare it on.
/// @param canonical The canonical long name, `--` included.
/// @param description The help line. Required: a call without one does not
/// compile. Pass `k_undocumented` only where no text has been authored yet.
/// @return The created option.
export auto add_bool_flag(CLI::App& app, std::string_view canonical, std::string_view description) -> CLI::Option*;

/// @brief Install, on `root` and every node beneath it, the help formatter
/// that keeps synthesized negations OUT of the rendered help page.
///
/// ## Why help must not grow the negations
///
/// CLI11's default formatter renders a flag with negation names as
///
///     --json, --no-json{false}
///
/// where it previously rendered `--json`. That is a change to ~223 leaf
/// help pages as a side effect of a parser fix, it leaks CLI11's internal
/// `{default-flag-value}` notation into operator-facing output, and the
/// ORACLE's help renders none of it — etcli synthesizes the negation in
/// its parser and never mentions it in its help. Declaring the negation is
/// meant to make the two binaries agree at the PARSER, not to make the
/// help pages disagree.
///
/// So the negation is accepted, absent from the schema catalog (see
/// `planar.cliapp.schema::aliases_of`), and absent from help — three
/// surfaces, one answer, all three matching the oracle.
///
/// ## Subcommand lists show summaries
///
/// The same formatter lists each child of a command by its summary: the
/// first paragraph of the child's description, collapsed to one line and
/// wrapped to the right column. A group's full prose stays on the group's
/// own `--help` page, so a parent's list stays short.
///
/// Must be called AFTER the tree is fully declared: CLI11 hands each
/// subcommand the formatter its parent held AT `add_subcommand` TIME, so a
/// formatter installed on the root before the children exist reaches only
/// the root.
/// @param root The fully declared tree.
export auto hide_negations_in_help(CLI::App& root) -> void;

/// @brief Reorder `argv` so every subcommand token precedes every flag and
/// positional, reproducing etcli's POSITION-INDEPENDENT flag handling
/// (plan 996, task 6131 item 1).
///
/// ## What the oracle actually does, derived by running it
///
/// Task 6131 recorded this as "a global flag BEFORE the subcommand" and
/// suspected a root-level declaration. It is neither. `planar --json
/// health` works and `planar --json version` does NOT:
///
///     planar --json health          exit 0, the health JSON
///     planar --json version         exit 2, error: unknown flag (got --json)
///     planar --bogus health         exit 2, error: unknown flag (got --bogus)
///     planar workbench --json list  exit 0  (MID-PATH, not merely leading)
///     planar --scope oracle task list
///                                   exit 1, SlugNotFound — so `oracle` was
///                                   read as --scope's VALUE, not as a verb
///
/// `--json` is not accepted at the root; it is accepted because the leaf
/// `health` eventually resolved declares one. etcli's `parse`
/// (zig/vendor/etcli-zig/src/cli/parser.zig) makes ONE pass over argv,
/// routing every flag-shaped token into a `tail` buffer and continuing to
/// match subcommands with the tokens that remain. The leaf's own parser
/// then runs against `tail`. Position simply never enters into it.
///
/// So this is a pure ARGV REORDERING, and reproducing it costs the catalog
/// nothing — no flag is declared anywhere it was not already declared, and
/// an undeclared flag is still refused by the leaf, with the leaf's own
/// message. That is what makes it safe to match the oracle here rather
/// than record the divergence: the alternative fix, `allow_extras` on the
/// root, would have accepted `--bogus` too.
///
/// ## The relative order of tail tokens is preserved, and must be
///
/// etcli appends flags, flag values and positionals to ONE buffer in
/// encounter order, and that is load-bearing rather than incidental: a
/// non-bool flag's value is recognised as a value only because it lands
/// immediately after its flag. `--scope oracle` survives the reorder for
/// exactly that reason, with no need to know that `--scope` takes a value.
/// @param root The fully declared tree.
/// @param argv The full process argv, `argv[0]` included.
/// @return The reordered argv: `argv[0]`, then the resolved subcommand
/// path, then every other token in its original relative order.
export auto hoist_subcommands(const CLI::App& root, std::span<std::string const> argv) -> std::vector<std::string>;

} // namespace planar::cliapp
