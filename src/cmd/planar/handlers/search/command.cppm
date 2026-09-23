/// @file src/cmd/planar/handlers/search/command.cppm
/// @brief `planar.cmd.planar.handlers.search` — the `planar search` leaf
/// (plan 996, task 6090).
///
/// Port of zig/src/cmd/planar/handlers/search.zig. Everything here is the
/// `cmd/`-layer half: flag validation, the cross-scope merge, and the two
/// output modes. The query itself is `planar.engine.search`.
///
/// ## Three flag conventions that LOOK alike and are not
///
/// All three were captured from the oracle, not inferred, because the
/// naive reading gets two of them wrong:
///
///   - `--scope ""` means CROSS-SCOPE. The handler maps an empty string to
///     "no scope filter", so `search zephyr --scope ""` returns hits from
///     every scope, exit 0.
///   - `--status ""` means MATCH NOTHING. The same empty string on the
///     sibling flag is passed through as a one-element status list, and
///     `status IN ('')` matches no row — so the verb prints `(no results)`,
///     exit 0. It is NOT "any status". The asymmetry with `--scope` is the
///     oracle's and is reproduced deliberately.
///   - `--status bogus` also prints `(no results)`, exit 0. Statuses are
///     NOT validated against any vocabulary, unlike `--kind`.
///
/// `--kind` is the one that DOES validate: an unrecognised value refuses at
/// exit 2 with `unknown kind '<value>'` rather than matching nothing. That
/// asymmetry is worth keeping — a typo'd kind would otherwise be
/// indistinguishable from a genuine no-match.
///
/// ## Absent `--scope` resolves the cwd READ SET, and refuses when empty
///
/// With no `--scope` the handler resolves the cwd-derived read set and runs
/// the engine query ONCE PER member scope, then merges. An EMPTY read set
/// refuses at exit 1 (`cwd is not inside any registered Planar scope; …`)
/// rather than falling through to an unfiltered search — the silent-filter
/// defect this milestone keeps closing.
///
/// The merge re-sorts by `(rank DESC, kind ASC, id ASC)` — the engine's own
/// order, reapplied because per-scope result sets are individually sorted
/// but their concatenation is not — and then truncates to the limit. The
/// truncation is applied to the MERGED list, so a two-scope read set with
/// `--limit 5` returns five rows total, not five per scope.
///
/// ## `rank` is rendered through the tree's shared float formatter
///
/// `planar.json_text`'s `append_json_double` — shortest round-trip digits
/// in FIXED notation, so an exponent never appears (`0.000001375`, never
/// `1.375e-06`), and a non-finite rank would be `null` rather than a bare
/// `inf`. This file used to carry its own `format_zig_float`, which was one
/// of four transcriptions of that algorithm and was BROKEN for every rank
/// with a non-negative exponent — the common case. See search.cpp for the
/// measurement and json_text.cppm for the inventory.
module;

export module planar.cmd.planar.handlers.search;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {

/// @brief Handle `planar search <query> [--kind --status --scope --plan
/// --limit --json]`.
///
/// @param ctx The process context (database handle, streams, cwd).
/// @param args The parsed command line.
/// @return Success after writing the payload, or the refusal.
export auto search(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `search` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_search(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
