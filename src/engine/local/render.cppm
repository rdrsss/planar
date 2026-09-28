/// @file render.cppm
/// @brief `planar.engine.local.render` — byte-exact renderers for all five
/// `planar local` leaves (plan 996, task 6109).
///
/// Behavior-preserving port (D2) of the rendering half of
/// zig/src/cmd/planar/handlers/local/{list,link,unlink,import,migrate,common}.zig.
///
/// ## Three DIFFERENT JSON envelope conventions in one verb family
///
/// This is the single most surprising thing about `local`, and every part of it
/// was established by capturing bytes rather than reading a schema:
///
///   `list`, `link`, `link --reconcile`  NDJSON — one complete object per line,
///                                       no enclosing array, no commas.
///   `unlink`                            NDJSON, but each line is wrapped in a
///                                       `{"result":{...}}` envelope the other
///                                       two do not have.
///   `import`, `migrate`                 ONE object for the whole run.
///
/// The consequence for empty input is that the three families disagree, and all
/// three spellings are pinned:
///
///   `local list --json`            -> ZERO BYTES, exit 0
///   `local link --reconcile --json`-> ZERO BYTES, exit 0
///   `local migrate --json`         -> `{"Migrated":[],"Skipped":[]}\n`
///
/// A renderer emitting `[]` for the first two would be valid JSON and a parity
/// break.
///
/// ## Go-shaped PascalCase keys, and where they stop
///
/// The JSON carries `Name` / `Kind` / `Record` / `Migrated` / `Imported` —
/// PascalCase left over from the Go implementation this was ported from. But it
/// stops one level down: inside `Record` the keys are snake_case (`vendor`,
/// `target_path`, `linked_at`). The mixture is not a mistake to tidy; it is the
/// wire format. `import` and `migrate` are PascalCase all the way through.
///
/// ## Null omission is per-call-site, not global
///
/// `link` / `list` / `unlink` render with `emit_null_optional_fields = false`,
/// so a null `warning` or `linked_at` is ABSENT from the object rather than
/// `null`. Oracle-confirmed: a `skipped` record emits neither key, while its
/// siblings emit both. `import` and `migrate` use no such option and emit every
/// field unconditionally — their `Reason` is `""`, never absent.
///
/// A null `mode` renders as the EMPTY STRING `""`, not as absent and not as
/// null, because the Zig handler stringifies it as `if (m) |x| @tagName(x) else ""`
/// before serialization ever sees it.
///
/// ## Text-mode column widths
///
/// Fixed-width, padded with spaces, never truncated — an over-long value pushes
/// the row right. Widths differ per leaf and are transcribed from the Zig format
/// strings: `list` uses 12/7/7/7, `link` uses 7 for the vendor, `import` and
/// `migrate` use 12 and 20 respectively.

module;

export module planar.engine.local.render;

import std;
import planar.engine.local.manifest;
import planar.engine.local.link;
import planar.engine.local.importer;

namespace planar::engine::local::render {

/// @brief Render `local list --json` (NDJSON).
///
/// EMPTY for an empty list — not `[]`. See this module's header.
/// @param rows The rows, already sorted.
/// @param vendor_filter `--vendor`; rows whose vendor differs are dropped
/// entirely rather than rendered as skipped.
/// @return One line per surviving row.
export auto list_json(std::span<const link::list_record> rows, std::optional<std::string_view> vendor_filter) -> std::string;

/// @brief Render `local list` (no `--json`).
///
/// An empty list is the sentence `no sandbox installs recorded`. A NON-empty
/// list that the vendor filter empties is a DIFFERENT sentence,
/// `no rows matched filter`, printed AFTER the header row — which is therefore
/// still emitted. Both are oracle-captured and the distinction is deliberate:
/// "you have nothing" and "you have things, none matching" are different
/// operator situations.
/// @param rows The rows, already sorted.
/// @param vendor_filter `--vendor`.
/// @return The complete stdout payload.
export auto list_text(std::span<const link::list_record> rows, std::optional<std::string_view> vendor_filter) -> std::string;

/// @brief Render one `local link` / `local import` JSON line.
///
/// The `{"Source":{...},"Records":[...]}` shape, one line per SOURCE (not per
/// target). `Source.Frontmatter.Vendors` is always present, as `[]` when the
/// author named none — the resolved all-three list is NOT substituted here, so
/// the JSON shows what was written rather than what was used.
/// @param file The parsed source.
/// @param result What link() did.
/// @return One JSON object, newline-terminated.
export auto link_json(const manifest::sandbox_file& file, const link::link_result& result) -> std::string;

/// @brief Render one source's `local link` text block.
///
/// `<name> (<kind>)` then one indented line per record. A record with a mode
/// prints `<action> [<mode>]`; one without (a `skipped` record) prints just the
/// action, with the bracket group and its surrounding space ABSENT — not an
/// empty `[]`.
/// @param result What link() did for one source.
/// @return The block, newline-terminated.
export auto link_source_text(const link::link_result& result) -> std::string;

/// @brief Render the `local link` trailing summary line.
///
/// Two entirely different sentences. Dry-run counts only `dry-run` records;
/// the real run counts created+updated as "linked" and reports unchanged and
/// skipped separately. Both are preceded by a blank line.
/// @param results Every source's result.
/// @param dry_run Which sentence to emit.
/// @return A blank line then the summary, newline-terminated.
export auto link_summary_text(std::span<const link::link_result> results, bool dry_run) -> std::string;

/// @brief Render a walk error as `local link` prints it.
///
/// Emitted in TEXT MODE ONLY — under `--json` the walk errors are silently
/// dropped, so a malformed sandbox source is invisible to a scripted caller.
/// That asymmetry is the oracle's and is pinned rather than corrected.
/// @param value The walk error.
/// @return `warning: <path>: <message>`, newline-terminated.
export auto walk_error_text(const manifest::walk_error& value) -> std::string;

/// @brief Render a lint issue as `local link` prints it.
///
/// `lint [<severity>] <kind>/<name>.<field>: <message>` — note the severity is
/// the Zig enum name, so the reserved `error` variant would render as `error`
/// rather than this tree's `error_` spelling.
/// @param issue The finding.
/// @param file_kind The source's kind.
/// @param name The source's name.
/// @return The line, newline-terminated.
export auto lint_text(const manifest::lint_issue& issue, manifest::kind file_kind, std::string_view name) -> std::string;

/// @brief Render one `local unlink` JSON line.
///
/// The `{"result":{...}}` wrapper is unique to this leaf. `PurgedFile` is always
/// present, as `""` without `--purge`.
/// @param result What unlink() did.
/// @return One JSON object, newline-terminated.
export auto unlink_json(const link::unlink_result& result) -> std::string;

/// @brief Render one `local unlink` text block.
/// @param result What unlink() did.
/// @return The block, newline-terminated.
export auto unlink_text(const link::unlink_result& result) -> std::string;

/// @brief The sentence `local unlink` prints when nothing was removed.
///
/// Deliberately ambiguous between "already unlinked" and "no such name" — the
/// command cannot tell the two apart, and says so rather than guessing.
/// @param name The name that was not found.
/// @return The line, newline-terminated.
export auto unlink_none_text(std::string_view name) -> std::string;

/// @brief Render `local import --json` (one object for the whole run).
///
/// All three arrays are always present, `[]` when empty, and every record field
/// is emitted including an empty `Reason`.
/// @param value The import outcome.
/// @return One JSON object, newline-terminated.
export auto import_json(const import_::result& value) -> std::string;

/// @brief Render `local import` (no `--json`).
///
/// A run with nothing at all is the single sentence `no files matched for
/// import`. Otherwise: imported rows, skipped rows, warnings, a blank line, and
/// a count. NOTE the count sentence says `imported N file(s)` even under
/// `--dry-run`, where the per-row action reads `would-import` — the Zig handler
/// takes `dry_run` as a parameter and then discards it (`_ = dry_run;`).
/// Preserved.
/// @param value The import outcome.
/// @return The complete stdout payload.
export auto import_text(const import_::result& value) -> std::string;

/// @brief Render `local migrate --json`.
///
/// IDENTICAL under `--dry-run` — the JSON carries no dry-run marker at all, so a
/// scripted caller cannot tell a rehearsal from a real run. Oracle-confirmed by
/// diffing the two captures byte for byte.
/// @param value The migrate outcome.
/// @return One JSON object, newline-terminated.
export auto migrate_json(const manifest::migrate_result& value) -> std::string;

/// @brief Render `local migrate` (no `--json`).
///
/// Nothing to do is `migrate: no legacy flat skills found; sandbox is already
/// dir-shape`. Otherwise the per-row verb switches between `would migrate` and
/// `migrated` on `--dry-run`, and the same verb is reused in the summary line.
/// @param value The migrate outcome.
/// @param dry_run Which verb to use.
/// @return The complete stdout payload.
export auto migrate_text(const manifest::migrate_result& value, bool dry_run) -> std::string;

/// @brief Render `local link --reconcile --json` (NDJSON).
///
/// snake_case keys throughout — this one leaf does NOT use the PascalCase
/// convention its siblings do, because it is rendered from an anonymous struct
/// rather than a Go-shaped mirror. EMPTY for no actions.
/// @param actions What reconcile() did.
/// @return One line per action.
export auto reconcile_json(std::span<const link::reconcile_action> actions) -> std::string;

/// @brief Render `local link --reconcile` (no `--json`).
///
/// Every action prints as `removed N install(s)`, INCLUDING `target-missing`
/// actions where the installs were re-created rather than removed. That is the
/// oracle's wording; see link.cppm for why the underlying field is named
/// `removed_targets` in both cases.
/// @param actions What reconcile() did.
/// @param dry_run Switches `removed` to `would remove` and the summary verb.
/// @return The complete stdout payload.
export auto reconcile_text(std::span<const link::reconcile_action> actions, bool dry_run) -> std::string;

/// @brief The sentence `local link` prints when the sandbox holds no sources.
///
/// Exit 0, not an error — an operator with no personal skills yet is not in a
/// failure state. Contrast `local import` on an empty directory, which DOES
/// fail. The two leaves genuinely disagree.
/// @param sandbox_root The root that was searched, interpolated into the line.
/// @return The line, newline-terminated.
export auto no_sources_text(std::string_view sandbox_root) -> std::string;

/// @brief The error line `local link <name>` emits for an unknown name.
/// @param name The name that did not resolve.
/// @param sandbox_root The root that was searched.
/// @return `error: no sandbox source named "<name>" under <root>`, newline-terminated.
export auto no_such_source_error(std::string_view name, std::string_view sandbox_root) -> std::string;

/// @brief The error line `local import --kind <bad>` emits.
///
/// Note the value is DOUBLE-QUOTED here while no_such_source_error() also
/// double-quotes but the reconcile refusal does not quote at all. The three are
/// pinned separately precisely because a shared helper would smooth over a
/// difference the oracle actually has.
/// @param raw The rejected `--kind` value; empty when the flag was absent.
/// @return The line, newline-terminated.
export auto invalid_kind_error(std::string_view raw) -> std::string;

/// @brief The error line `local link --reconcile <name>` emits.
/// @return The line, newline-terminated.
export auto reconcile_takes_no_positional_error() -> std::string;

} // namespace planar::engine::local::render
