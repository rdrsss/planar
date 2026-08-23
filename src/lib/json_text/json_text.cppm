/// @file json_text.cppm
/// @brief `planar.json_text` — the one JSON string-escaping primitive every
/// hand-rolled renderer in the tree shares (plan 996, task 6109/6110/6111).
///
/// ## Why this is a layer-1 module and not an eleventh private copy
///
/// D18 forbids `engine_*` -> `engine_*` edges, so a helper two engine buckets
/// both need has exactly two legal shapes: duplicated per bucket, or extracted
/// downward into a layer-1 base library (D19 — the same move that produced
/// `scope_ref`). Duplication was the standing choice, and
/// `engine/workflows/render.cpp` wrote the rule down explicitly: extracting is
/// "worth doing once a THIRD bucket needs it rather than on the second".
///
/// That threshold had been passed several times over when it was written. The
/// tree carried TEN definitions of this function under two different names:
///
///   append_json_string (7)          json_quote (3)
///     engine/closure/store.cpp        engine/planning/annotation.cpp
///     engine/grouping/load.cpp        engine/runtime/capture.cpp
///     engine/ingest/render.cpp        engine/promotion/promotion.cpp
///     engine/models/render.cpp
///     engine/planning/test_spec_status.cpp
///     engine/runs/render.cpp
///     engine/workflows/render.cpp
///
/// The seven `append_json_string` copies really were behaviorally identical
/// (two spell the C0 test `c < 0x20` and five spell it `c <= 0x1F`, which is
/// the same predicate). The three `json_quote` copies were NOT: they sent 0x08
/// and 0x0C to the `\u00xx` arm instead of emitting the short forms, which is
/// a live parity break wherever the quoted field carries operator free text or
/// extracted source. See json_text/CMakeLists.txt for the full account.
///
/// All ten now import this module. There is exactly one definition.
///
/// ## The escaping table is a parity contract, not a style choice
///
/// Every JSON surface in Planar is pinned byte-for-byte against the Zig
/// oracle's `std.json.Stringify`, so the escape set is fixed by what that
/// emits and nothing else:
///
///   - `\\` and `\"` — the two mandatory escapes.
///   - `\b` `\f` `\n` `\r` `\t` — the five short forms.
///   - `\u00xx` with LOWERCASE hex for every other byte below 0x20. Uppercase
///     would still be valid JSON and would still be a parity break.
///   - `/` is NOT escaped. Every `target_path` / `source_path` field in the
///     `local` bucket is a filesystem path, so an escaping `/` would corrupt
///     essentially every record this tree emits.
///   - Bytes >= 0x20 pass through verbatim, so non-ASCII stays raw UTF-8
///     rather than becoming `\uXXXX` surrogate pairs.
///   - DEL (0x7F) is NOT escaped — it is >= 0x20 and takes the default arm.
///
/// The function is byte-oriented, not codepoint-oriented: it never decodes
/// UTF-8, which is exactly why invalid UTF-8 passes through unchanged instead
/// of throwing. The oracle does the same.

module;

export module planar.json_text;

import std;

namespace planar::json_text {

/// @brief Append `text` to `out` as a quoted, escaped JSON string.
///
/// Writes the surrounding double quotes itself, so the caller appends only the
/// separators around it. See this module's header for the exact escape table
/// and why each entry is a parity contract rather than a preference.
/// @param out Destination buffer; appended to, never cleared.
/// @param text The raw bytes to quote. Not required to be valid UTF-8.
export auto append_json_string(std::string& out, std::string_view text) -> void;

/// @brief Return `text` as a quoted, escaped JSON string.
///
/// Convenience wrapper over append_json_string() for the call sites that build
/// a value in isolation rather than appending into a running buffer.
/// @param text The raw bytes to quote.
/// @return The quoted, escaped string, including both quote characters.
export auto json_string(std::string_view text) -> std::string;

} // namespace planar::json_text
