/// @file output.cppm
/// @brief `planar.engine.templates.output` — byte-exact stdout/stderr
/// payloads for all six `planar templates` leaves (plan 996, task 6190).
///
/// Behavior-preserving port (D2) of the rendering half of
/// `zig/src/cmd/planar/handlers/templates/{list,show,render,validate,init,path}.zig`.
///
/// Named `output` rather than `render` because this bucket's `render`
/// module is already the TEMPLATE SUBSTITUTION engine. The two are
/// genuinely different jobs — one turns `{{.Task.Title}}` into a task
/// title, the other turns a row into a line of stdout — and giving them the
/// same name inside one bucket would be a coin flip at every import.
///
/// ## Every empty-output spelling in this family is different
///
/// This is the part a plausible implementation gets wrong, and all four
/// were captured from the built oracle rather than assumed:
///
///   `templates list` (no matches)        -> `no templates found\n`
///   `templates list --json` (no matches) -> ZERO BYTES, exit 0
///   `templates init` (all present)       -> `templates init: nothing to do (all templates already present)\n`
///   `templates init --json` (all present)-> ZERO BYTES, exit 0
///
/// Both `--json` leaves are NDJSON — one object per line, no enclosing
/// array, no commas — so "nothing" is genuinely zero bytes rather than
/// `[]`. Emitting `[]` would be valid JSON and a parity break.
///
/// ## Two leaves ignore flags they declare
///
/// `templates path` accepts `--system`, `--set` and `--json` and uses NONE
/// of them: it prints the resolved root as bare text under every
/// combination. `templates init` accepts `--force` and discards it, so it
/// never overwrites. Both confirmed against the oracle. These are
/// reproduced rather than corrected — see this bucket's CMakeLists for why
/// a "sensible" `--force` here would be a unilateral divergence — but a
/// caller must not assume the flags do anything.
///
/// ## `templates show` does NOT round-trip the JSON
///
/// It writes the template's RAW BYTES, so an operator's own formatting
/// survives verbatim, and a newline is appended ONLY when the file does not
/// already end in one. Passing it through the DOM would re-indent
/// `"labels": ["planar-managed", "type:task"]` onto four lines and change
/// the output of a leaf whose whole purpose is showing the file as
/// authored.
///
/// ## `validate`'s issue lines go to STDOUT, and the summary to STDERR
///
/// The `ISSUE:` lines and the `--json` envelope are stdout; the
/// `N issue(s) in set/system/kind` summary is stderr, at exit 2 — the
/// `invalid_input` bucket, NOT exit 1. A caller that reads only stderr sees
/// the count and not the detail, and a caller that reads only stdout sees
/// the detail and not the failure. Both halves are pinned.
///
/// ## Fixed-width columns, never truncated
///
/// `list`'s text mode pads SET/SYSTEM/KIND to 20 characters and lets an
/// over-long value push the row right rather than clipping it. The rule
/// under the header is exactly 70 dashes.

module;

export module planar.engine.templates.output;

import std;
import planar.engine.templates.validate;

namespace planar::engine::templates {

/// @brief One row of `templates list`.
///
/// The loader's own `config::template_entry` is NOT used here, and not
/// because the shapes differ — `engine_config` and `engine_templates` are
/// both LAYER 2, and `cmake/architecture.cmake` FATALs on an engine→engine
/// edge. The cmd layer, which may import both, does the one-line mapping.
///
/// `source` is a plain string rather than an enum for the same reason, and
/// it holds the literal word the leaf prints: `"disk"` or `"embedded"`.
export struct list_row {
  std::string set_name; ///< The template set.
  std::string system;   ///< The external-system slug.
  std::string kind;     ///< The kind within the set.
  std::string source;   ///< Literally `"disk"` or `"embedded"`.
  std::string path;     ///< The filesystem path, or `embedded:<system>/<kind>.json`.
};

/// @brief Merge disk over embedded entries, apply the filters, and sort.
///
/// A disk entry SUPERSEDES an embedded entry with the same
/// `(set, system, kind)` triple — so after `templates init` every row reads
/// `disk` rather than `embedded`, which is the visible signal that the
/// operator now owns those files.
///
/// An EMPTY filter string means NO FILTER, not "match the empty string".
/// `templates list --system ""` therefore lists everything — captured from
/// the oracle, whose handler guards each filter with a `len > 0` check.
/// Callers pass `std::nullopt` and `""` interchangeably on purpose.
/// @param disk Disk-discovered entries.
/// @param embedded Embedded entries.
/// @param system_filter `--system`, if given.
/// @param set_filter `--set`, if given.
/// @return The surviving rows, sorted by `(set, system, kind)`.
export auto merge_list_rows(std::span<const list_row> disk, std::span<const list_row> embedded,
                            std::optional<std::string_view> system_filter, std::optional<std::string_view> set_filter)
    -> std::vector<list_row>;

/// @brief Render `templates list` (text).
/// @param rows The merged, filtered, sorted rows.
/// @return A header, a 70-dash rule and one padded line per row — or the
/// single sentence `no templates found\n` when there are none.
export auto list_text(std::span<const list_row> rows) -> std::string;

/// @brief Render `templates list --json` (NDJSON).
/// @param rows The merged, filtered, sorted rows.
/// @return One object per line; ZERO BYTES when there are none.
export auto list_json(std::span<const list_row> rows) -> std::string;

/// @brief Render `templates show`.
/// @param raw The template's verbatim bytes.
/// @return `raw`, with a newline appended only if it lacks a trailing one.
export auto show_text(std::string_view raw) -> std::string;

/// @brief Render `templates init` (text).
/// @param created The paths newly written, in the order they were written.
/// @return A count line plus a two-space-indented path per file, or the
/// `nothing to do` sentence when `created` is empty.
export auto init_text(std::span<const std::string> created) -> std::string;

/// @brief Render `templates init --json` (NDJSON).
/// @param created The paths newly written.
/// @return One `{"path":…}` object per line; ZERO BYTES when empty.
export auto init_json(std::span<const std::string> created) -> std::string;

/// @brief Render `templates validate`'s STDOUT for a clean template.
/// @param set_name The set, as REQUESTED (not as resolved — see below).
/// @param system The external-system slug.
/// @param kind The kind.
/// @param json Whether `--json` was given.
/// @return The `ok:` line, or the `"ok":true` envelope.
export auto validate_ok(std::string_view set_name, std::string_view system, std::string_view kind, bool json) -> std::string;

/// @brief Render `templates validate`'s STDOUT for a template with issues.
///
/// The `set_name` reported is the one the OPERATOR ASKED FOR, even when the
/// template actually resolved to the embedded default under a different
/// set. `templates validate myset jira epic --json` on a machine with no
/// `myset` on disk reports `"set":"myset"`. Oracle-captured; the loader
/// stamps the requested set onto the embedded result.
/// @param set_name The set, as requested.
/// @param system The external-system slug.
/// @param kind The kind.
/// @param path The resolved template's path — text mode leads every line
/// with it.
/// @param issues The issues, in document order.
/// @param json Whether `--json` was given.
/// @return The `ISSUE:` lines, or the `"ok":false` envelope.
export auto validate_issues(std::string_view set_name, std::string_view system, std::string_view kind, std::string_view path,
                            std::span<const validation_issue> issues, bool json) -> std::string;

/// @brief The STDERR summary that accompanies `validate_issues`.
/// @param count How many issues were found.
/// @param set_name The set, as requested.
/// @param system The external-system slug.
/// @param kind The kind.
/// @return The message BODY, with no `error: ` prefix and no terminator —
/// the cmd layer composes both.
export auto validate_summary(std::size_t count, std::string_view set_name, std::string_view system, std::string_view kind)
    -> std::string;

/// @brief The `template <set>/<system>/<kind> not found` message body.
/// @param set_name The set, as requested.
/// @param system The external-system slug.
/// @param kind The kind.
/// @return The body, with no prefix and no terminator.
export auto not_found_message(std::string_view set_name, std::string_view system, std::string_view kind) -> std::string;

} // namespace planar::engine::templates
