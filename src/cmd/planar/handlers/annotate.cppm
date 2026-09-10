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

/// @brief Handle `planar annotate show <annotation-id> [--json]`.
///
/// The positional is declared a STRING in the tree and parsed here, so a
/// non-integer refuses with the oracle's own wording and exit 2 rather than
/// a CLI11 type error at exit 1 — `entity_id_arg`'s contract. The engine
/// also offers `show_by_slug`, and this leaf deliberately does NOT reach
/// for it: `annotate show my-slug` against the oracle exits 2 with
/// `annotation id must be an integer, got 'my-slug'`, so accepting a slug
/// here would be a silent capability the oracle does not have.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// the mapped engine failure.
export auto annotate_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate update <annotation-id> [patch] [--json]`.
///
/// An all-unset patch is a legal no-op that re-renders the row WITHOUT
/// bumping `updated_at` — oracle-confirmed, and the engine's `update`
/// documents the same. There is no terminal-status guard: the oracle
/// updates an `archived` row's title happily.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or a
/// `generic_failure` (exit 1) for an unrecognized `--status`, or the mapped
/// engine failure.
export auto annotate_update(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate remove <annotation-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// the mapped engine failure.
export auto annotate_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate tag <annotation-id> <tag> [--remove] [--json]`.
///
/// The one leaf in this family whose `NotFound` is NOT rewritten: the
/// oracle answers `annotate tag 99 q` with `error: annotate tag: NotFound`
/// where every sibling answers `error: no annotation with id 99`. Both
/// shapes were captured; the difference is preserved rather than
/// normalised.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id or a
/// missing tag positional, or the mapped engine failure.
export auto annotate_tag(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate resolve <annotation-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// the mapped engine failure (`TerminalStatus` at exit 1 for a row already
/// at an outcome state).
export auto annotate_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate dismiss <annotation-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// the mapped engine failure.
export auto annotate_dismiss(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate archive <annotation-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// the mapped engine failure.
export auto annotate_archive(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate bulk-resolve [filters] [--json]`.
///
/// Passes `status_ = active` into the filter, which the engine's
/// `bulk_apply` documents as this leaf's contract. That is NOT a default
/// this handler is free to omit: dropping it would let `bulk-resolve` walk
/// `resolved` and `dismissed` rows, and the pass is not transactional, so
/// the damage would already be committed when the count came back wrong.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the mapped engine failure.
export auto annotate_bulk_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate bulk-dismiss [filters] [--json]`.
/// Passes `status_ = active`; see `annotate_bulk_resolve`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the mapped engine failure.
export auto annotate_bulk_dismiss(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate bulk-archive [filters] [--json]`.
///
/// The one bulk leaf that passes NO status filter, because `resolved` and
/// `dismissed` legally progress to `archived`. Oracle-confirmed: with four
/// `resolved` rows, `bulk-archive --tag x` reported `count: 3` — the three
/// carrying the tag — while `bulk-resolve` on the same set reported 0.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the mapped engine failure.
export auto annotate_bulk_archive(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate verify [--anchor-path P] [--scope S] [--json]`.
///
/// Lists the ACTIVE annotations matching the filter, reads each anchor's
/// file RELATIVE TO THE OPERATOR CWD, and classifies it through the
/// engine's pure `classify_anchor`. Filesystem access lives here rather
/// than in the engine by that function's own design note.
///
/// The cwd-relative resolution is oracle-derived rather than assumed:
/// running `annotate verify` from a subdirectory of the same project turned
/// every `fresh` row `stale`, which only happens if the path is joined to
/// the process cwd and not to the association root.
///
/// Its failure wording is also this family's odd one out — `annotate
/// verify --scope nosuch` answers `annotate verify: list failed:
/// SlugNotFound`, with the extra `list failed: ` segment no sibling has.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the mapped engine failure.
export auto annotate_verify(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar annotate sweep [--since-days N] [--scope S] [--json]`.
///
/// **`--scope` is declared on this leaf and the oracle does not apply it.**
/// `annotate sweep --scope nosuch` exits 0 reporting `swept: 0` where every
/// sibling exits 1 with `SlugNotFound`, and a sweep run with a `--scope`
/// naming a DIFFERENT association still archives this one's rows. The
/// engine's `sweep` takes no scope parameter either, so the port and the
/// oracle agree. Reproduced here deliberately — the oracle is the spec —
/// and filed as a defect against the oracle rather than silently "fixed",
/// because fixing it here would make the C++ binary diverge on a verb whose
/// whole purpose is bulk mutation.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the mapped engine failure.
export auto annotate_sweep(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Apply one receipt-backed structured annotation command.
export auto annotate_command(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Look up a receipt after an uncertain writer outcome.
export auto annotate_receipt(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `annotate` command tree on `root`.
///
/// The CLI declaration for every `annotate` node, colocated with the
/// handlers above (plan 1051, M11.3b — decision 1068). `add` and `list`
/// were previously hand-declared in `tree.cpp` and the other twelve came
/// from `surface.cpp`'s generated table; the two halves are ONE list here,
/// in catalog order, which is the invariant this declaration carries.
/// @param root The root app to attach the `annotate` group to.
export auto declare_annotate(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
