/// @file annotate.cppm
/// @brief `planar.cmd.planar.handlers.annotate` — the `planar annotate add`
/// and `planar annotate list` leaves (plan 996, task 6105).
///
/// Port target: zig/src/cmd/planar/handlers/annotate/{add,list}.zig plus
/// the JSON half of that directory's render.zig.
///
/// ## Why this pair is the layer-3 composition proof
///
/// D20 (decision 947) puts a verb that composes several layer-2 peers at
/// layer 3, and that is the whole reason this milestone exists — `unlink`,
/// `dashboard`, `report`, `skills`, `workspace init` and the ingest
/// apply/import/synthesize trio all blocked on a layer that did not exist.
/// `annotate add` is the smallest verb in the tree that is genuinely that
/// shape:
///
///   engine_identity  — `scope::resolve_for_write` derives the write scope
///                      from the cwd (or takes the `--scope` override)
///   engine_planning  — `annotation::create` writes the row
///
/// Those two buckets CANNOT reach each other. D18 forbids the edge and
/// `cmake/architecture.cmake` FATALs at configure time if either
/// CMakeLists tries. Composing them is not a convenience here; it is
/// structurally impossible anywhere below this file. That makes this pair
/// the honest end-to-end proof that layer 3 does the job it was created
/// for, in a way `version` and the two `workflow` leaves (single-bucket,
/// no database at all) cannot.
///
/// It also exercises what the other leaves skip: the lazy database
/// actually opening and migrating, and the `--json` / text split on a
/// renderer whose terminator contract is the OPPOSITE of `workflow`'s.
///
/// ## The terminator contract is per-renderer, and this is the counterexample
///
/// `planar.engine.planning.annotation`'s `render_json` and
/// `render_list_json` document themselves as returning "the single-line
/// JSON [object|array], WITH NO TRAILING NEWLINE" — fragments the caller
/// completes — while `render_text` and `render_list_text` say "Includes
/// trailing newlines". So this handler appends `'\n'` on the JSON path and
/// nothing on the text path, and `workflow list --json` (complete payload,
/// zero bytes when empty) appends nothing on either. Reading each
/// renderer's own `@return` is the rule; a blanket "always append" or
/// "never append" breaks one of the two.
module;

export module planar.cmd.planar.handlers.annotate;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar annotate add [flags]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) when `--anchor-path` is
/// missing, or the mapped engine failure.
export auto annotate_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate list [filters] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) for an unrecognized
/// `--status`, or the mapped engine failure.
export auto annotate_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
