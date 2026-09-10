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

// =========================================================================
// Doubles (plan 1006, tasks 6072/6186/6261)
// =========================================================================
//
// The tree carried THREE independent transcriptions of the same
// shortest-round-trip-in-fixed-notation algorithm, plus one site that had
// never implemented it at all:
//
//   src/cmd/planar/handlers/search.cpp   `format_zig_float`   (correct)
//   src/lib/engine/execute/manifest.cpp  `format_double`      (correct)
//   src/lib/json_dom/json_dom.cpp        `format_double`      (WRONG --
//       plain `std::to_chars` default, so `1.375e-06` where every other
//       site prints `0.000001375`; task 6261)
//   src/lib/engine/models/render.cpp     `json_number`        (WRONG --
//       `std::format("{}", v)`, scientific notation AND a bare `inf`;
//       tasks 6186/6261)
//
// `search.cpp`'s copy carried a standing note that json_dom "has the same
// divergence ... NOT reused here, and not fixed here either". That is the
// exact D19 shape json_text was extracted for. There is one definition now.

/// @brief Format `value` as shortest-round-trip digits in FIXED notation.
///
/// This is Zig's `{d}` / `std.fmt.format` spelling: the shortest decimal
/// digit string that round-trips through `double`, with the decimal point
/// placed by hand so an exponent NEVER appears. `1e300` is a `1` followed by
/// three hundred zeros; `1.375e-06` is `0.000001375`.
///
/// Neither half is what one `std::to_chars` call gives. The default (general)
/// format yields the shortest digits but switches to scientific for large and
/// small magnitudes; `chars_format::fixed` never uses an exponent but is
/// shortest only *for fixed notation*, so it spells out the full exact binary
/// expansion (`1e300` becomes 300 digits of noise). So: take the shortest
/// digits from the general form, then move the point.
///
/// NOT JSON-SAFE. Non-finite input returns Zig's bare spellings — `inf`,
/// `-inf`, `nan` — none of which is legal JSON. That is deliberate: the Lua
/// manifest surface in `engine/execute` renders these values outside a JSON
/// document and its pins require exactly those three strings. Every JSON
/// emitter must go through append_json_double() instead.
/// @param value The value to render.
/// @return The decimal text, with no exponent.
export auto format_double_fixed(double value) -> std::string;

/// @brief Append `value` to `out` as a JSON number, or `null` if non-finite.
///
/// The JSON-safe wrapper over format_double_fixed(). JSON has no spelling for
/// an infinity or a NaN, so this emits `null` for both rather than a bare
/// `inf` / `nan` token that no parser accepts.
///
/// A caller for whom `null` is the wrong answer must refuse the value BEFORE
/// it reaches an emitter — which is what `models evals --quality-floor` now
/// does (task 6186). This arm is the total-function safety net behind that
/// refusal, not a substitute for it: an emitter that cannot represent its
/// input must still produce a parseable document.
/// @param out Destination buffer; appended to, never cleared.
/// @param value The value to render.
export auto append_json_double(std::string& out, double value) -> void;

/// @brief Return `value` as a JSON number, or `null` if non-finite.
///
/// Convenience wrapper over append_json_double(), for the call sites that
/// build a value in isolation rather than appending into a running buffer.
/// @param value The value to render.
/// @return The number's bytes, or the four bytes `null`.
export auto json_double(double value) -> std::string;

} // namespace planar::json_text
