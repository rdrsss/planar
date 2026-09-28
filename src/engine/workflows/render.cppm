/// @file render.cppm
/// @brief `planar.engine.workflows.render` — byte-exact renderers for
/// `planar workflow list` and `planar workflow show` (plan 996, task 6096).
///
/// Behavior-preserving port (D2) of the rendering half of
/// zig/src/cmd/planar/handlers/workflow/{list,show}.zig.
///
/// ## `workflow list --json` emits NDJSON, not a JSON array
///
/// This is the single most surprising thing in the file and was established
/// by capturing bytes, not by reading a schema. Each entry is its own
/// complete JSON object on its own line, with NO enclosing `[`, no separating
/// commas, and no trailing `]`:
///
///     {"name":"aaa-partial","kind":"shipped",...}
///     {"name":"bare","kind":"shipped",...}
///
/// The consequence for HAZARD 6 (empty input) is that an empty catalog under
/// `--json` produces ZERO BYTES — not `[]`, not `{}`, not a newline.
/// Captured against a `PLANAR_HOME` with no `workflows/` directory at all:
/// exit 0, empty stdout. A renderer that emitted `[]` here would be valid
/// JSON and a parity break.
///
/// ## Field order
///
/// name, kind, path, filename, meta_found, description, phases, seam — the
/// Zig payload struct's declaration order, which `std.json.Stringify`
/// preserves.

module;

export module planar.engine.workflows.render;

import std;
import planar.engine.workflows.catalog;

namespace planar::engine::workflows::render {

/// @brief Render one entry as a single NDJSON line.
///
/// Used by both `workflow list --json` (once per entry) and
/// `workflow show <name> --json` (once, total) — the payload is IDENTICAL for
/// both leaves, oracle-confirmed byte for byte.
/// @param value The entry to render.
/// @return One JSON object, newline-terminated.
export auto entry_json(const catalog::entry& value) -> std::string;

/// @brief Render `workflow list --json` in full.
/// @param entries The catalog, already ordered.
/// @return One NDJSON line per entry; EMPTY for an empty catalog.
export auto list_json(std::span<const catalog::entry> entries) -> std::string;

/// @brief Render `workflow list` (no `--json`).
///
/// A fixed-width table: name in 24 columns, kind in 8, phases in 20, each
/// followed by TWO spaces, then the description unpadded. Over-long values
/// push the row right rather than truncating — oracle-confirmed with a
/// 48-character workflow name.
///
/// An empty catalog produces a sentence naming WHICH sources were searched,
/// so `--local` and the default read differently:
///   default:  `no shipped + sandbox workflows found`
///   --local:  `no sandbox workflows found`
/// @param entries The catalog, already ordered.
/// @param local_only Whether `--local` restricted the search; changes only
/// the empty-catalog sentence.
/// @return The complete stdout payload.
export auto list_text(std::span<const catalog::entry> entries, bool local_only) -> std::string;

/// @brief Render `workflow show <name>` (no `--json`).
///
/// Four always-present rows (name / kind / path / meta) followed by
/// description / phases / seam ONLY when non-empty. Omitting an empty field
/// rather than printing a bare `description:` is the oracle's behavior and is
/// what makes a metadata-less workflow render as four clean lines.
/// @param value The resolved entry.
/// @return The complete stdout payload.
export auto show_text(const catalog::entry& value) -> std::string;

/// @brief The error line `workflow show` and `workflow run` both emit for an
/// unresolvable name.
///
/// Shared deliberately: both leaves resolve through the same `catalog::find`
/// and the oracle emits the identical text for both (`workflow show nope` and
/// `workflow run nope --phase x` each exit 1 with this line). Keeping one
/// function makes it impossible for the two to drift — the task brief's
/// hazard 6 warns that two leaves reporting "not found" with different
/// quoting is exactly the kind of thing a shared helper gets wrong, so this
/// one is pinned against BOTH leaves' captures.
/// @param name The name that did not resolve.
/// @return `error: workflow '<name>' not found`, newline-terminated.
export auto not_found_error(std::string_view name) -> std::string;

} // namespace planar::engine::workflows::render
