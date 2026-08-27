/// @file json_dom.cppm
/// @brief `planar.json_dom` — an INSERTION-ORDERED JSON DOM whose parser
/// and writer both reproduce `std.json`'s exact behaviour, byte for byte
/// (plan 996, task 6190).
///
/// A LAYER-1 base library, beside `planar.json_text` (D19). It started
/// life inside `engine/templates` and was relocated the moment a
/// differential run showed the RESOLUTION half of the template plane
/// (`planar.engine.config.templates`, layer 2) needed the same parser to
/// stay faithful. Two layer-2 buckets cannot import each other, and the
/// alternative — a second, subtly-different validity check inside
/// `engine_config` — is exactly the duplication D19 exists to prevent.
/// So there is ONE JSON DOM in this tree and both halves use it.
///
/// ## Why the tree needs its own DOM rather than Glaze's
///
/// Because `templates render` writes the template's own KEY ORDER to
/// stdout, and Glaze re-sorts.
///
/// The oracle decodes with `std.json.parseFromSlice` into
/// `std.json.Value`, whose `ObjectMap` is a `StringArrayHashMap` —
/// INSERTION-ORDERED — and re-encodes by walking that map in the same
/// order. So `templates render default github-issues issue` prints
/// `title`, `body`, `labels`, `assignees`: the order they appear in
/// `templates/defaults/github-issues/issue.json`, which is NOT
/// lexicographic.
///
/// `glz::json_t` backs its object with `std::map<std::string, json_t>` —
/// SORTED. Round-tripping `issue.json` through it yields `assignees`,
/// `body`, `labels`, `title`: valid JSON, identical values, different
/// bytes, exit 0, and nothing in any parity lane would notice. That is the
/// silent-degradation shape this milestone keeps finding.
///
/// Glaze stays the right tool where the question is "is this valid JSON at
/// all" for a document nobody re-emits. It is the wrong tool for "hand me
/// back the document I gave you".
///
/// ## Four `std.json` behaviours that a plausible parser gets wrong
///
/// Every one of these was WRONG in the first draft of this file and was
/// corrected only because a differential run against the built oracle
/// disagreed. They are not incidental.
///
///   1. **DUPLICATE KEYS ARE AN ERROR.**
///      `std.json.ParseOptions.duplicate_field_behavior` defaults to
///      `.@"error"` (`std/json/static.zig:19-26`), so
///      `{"a":1,"a":2}` fails to parse. The draft implemented
///      "last value at the first position", reasoning from
///      `ObjectMap.put`. Operator-visible consequence: a template with a
///      repeated key is REJECTED by the loader and falls through the
///      resolution chain, so `templates show probe/px/dup` is
///      `error: template probe/px/dup not found` at exit 1 — not a
///      successful render of the last value.
///
///   2. **An integer too large for `i64` keeps its SOURCE TEXT.**
///      `Value.parseFromNumberSlice` (`std/json/dynamic.zig:30-47`) falls
///      back to `.number_string` on `error.Overflow`, and `Stringify`
///      prints that arm with `{s}` — verbatim. So
///      `12345678901234567890` round-trips EXACTLY. The draft converted it
///      to `double` and emitted `12345678901234567168`: silent precision
///      loss in a rendered payload that then gets pushed to Jira.
///      `json_kind::number_raw` is that arm.
///
///   3. **A float that overflows to infinity ALSO keeps its source text.**
///      Same function: `parseFloat` then `isFinite`, and a non-finite
///      result becomes `.number_string` rather than `inf` (which is not
///      legal JSON to emit).
///
///   4. **A finite float IS reformatted**, shortest-round-trip, matching
///      zig's `{d}`. So `1.50` comes back as `1.5` on both sides. Keeping
///      the raw text for every number — the obvious "just don't touch it"
///      fix for (2) — would break this one.
///
/// Beyond those: no comments, no trailing commas, no NaN/Infinity
/// literals, and trailing content after the top-level value is an error
/// (`std.json.parseFromSlice` requires the document to be exhausted;
/// `glz::read_json` does NOT, which was task 6086's finding).
///
/// ## The escape table is the oracle's, not a plausible one
///
/// Transcribed from `std/json/Stringify.zig`'s `outputSpecialEscape` and
/// `encodeJsonStringChars` (zig 0.16.0, lines 656-716) rather than from
/// memory — a first draft got two of these wrong:
///
/// Escape sequences below are spelled with the backslash written out as
/// the word, because doxygen reads a bare backslash-b or backslash-f in a
/// comment as its own bold/formula command and aborts the lint gate. The
/// authoritative table is `append_escaped`'s switch in json_dom.cpp.
///
///   - A backslash and a double quote take the two-character forms
///     (backslash-backslash and backslash-quote).
///   - **0x08 and 0x0C take the SHORT forms backslash-b and backslash-f**,
///     NOT a literal 0x08 / 0x0C byte. (The draft claimed the opposite.)
///   - Every OTHER byte below 0x20 becomes backslash-u followed by four
///     LOWERCASE hex digits, zero-padded.
///   - **Everything from 0x20 up passes through verbatim** except the
///     double quote and the backslash — DEL (0x7F) and every multi-byte
///     UTF-8 sequence included. `Options.escape_unicode` would change
///     that; it defaults to false and nothing here sets it.
///
/// That table is EXACTLY `planar.json_text`'s `append_json_string`, which
/// was transcribed from the same zig function for the same reason. So the
/// writer below CALLS it rather than carrying a ninth copy — the first
/// draft duplicated it "because the DOM has stricter requirements", which
/// was simply untrue: the two tables were byte-identical.
///
/// The oracle has one escaper too, not two: `output.writeJsonString` (used
/// by the `list` and `init` leaves) is a one-line forward to
/// `std.json.Stringify.encodeJsonString` with default options. Verified,
/// not assumed.

module;

export module planar.json_dom;

import std;

namespace planar::json_dom {

/// @brief The JSON value kinds this DOM distinguishes — one per live arm
/// of `std.json.Value`.
export enum class json_kind : std::uint8_t {
  null_,      ///< `null`.
  boolean,    ///< `true` / `false`.
  integer,    ///< An integer-formatted token that fits `std::int64_t`. Zig's `.integer`.
  floating,   ///< A number that parses to a FINITE double. Zig's `.float`.
  number_raw, ///< A number that fits neither — kept as source text and emitted verbatim. Zig's `.number_string`. See this file's
              ///< header, items 2 and 3.
  string,     ///< A JSON string. The ONLY kind the template renderer substitutes into.
  array,      ///< An ordered list.
  object,     ///< An INSERTION-ORDERED key/value map. See this file's header.
};

/// @brief One JSON value.
///
/// `object` members live in a `std::vector<std::pair<...>>` rather than a
/// map because insertion order IS the contract (see this file's header).
/// Lookup is linear; the documents this serves have single-digit key
/// counts, so an index would buy nothing and an ordered container is the
/// honest shape.
export struct json_value {
  /// @brief Which arm is live.
  json_kind kind = json_kind::null_;

  bool         boolean  = false; ///< Live when `kind == boolean`.
  std::int64_t integer  = 0;     ///< Live when `kind == integer`.
  double       floating = 0.0;   ///< Live when `kind == floating`.
  /// @brief Live when `kind == string` (the decoded text) or
  /// `kind == number_raw` (the verbatim numeric token).
  std::string string;

  std::vector<json_value>                         array;  ///< Live when `kind == array`.
  std::vector<std::pair<std::string, json_value>> object; ///< Live when `kind == object`, in insertion order.

  /// @brief Find a member by key.
  /// @param key The member name.
  /// @return A pointer to the member's value, or `nullptr` when absent or
  /// when this value is not an object.
  [[nodiscard]] auto find(std::string_view key) const -> const json_value*;
};

/// @brief Why a parse failed.
///
/// Deliberately ONE enumerator. Every caller in this tree treats any
/// failure the same way — the loader falls through to the next resolution
/// level, the renderer refuses — and a richer error set would invite a
/// caller to branch on a distinction the oracle does not make either
/// (zig's `load()` catches every parse error with a bare `catch {}`).
export enum class json_parse_error : std::uint8_t {
  malformed, ///< The bytes are not a single well-formed JSON document.
};

/// @brief WHY a parse failed, for the one caller that must tell them apart.
///
/// The coarse `json_parse_error` above stays the default, and the argument
/// for it still holds for every caller that only needs "did it parse".
/// `workspace routing show` is the exception: it interpolates zig's
/// `@errorName` into its stderr, so the three parse failures the oracle
/// distinguishes are three DIFFERENT stderr payloads. Oracle-captured
/// against a pinned scratch arena:
///
///     b''                     UnexpectedEndOfInput   (also `   `, `{`,
///                             `{"a":`, `{"a":"b`)
///     b'this is not json'     SyntaxError            (also `x{}`,
///                             `{} trailing`, `{"a":1,}`, `{'a':1}`,
///                             `{"a":NaN}`, a raw control byte in a string)
///     duplicate object key    DuplicateField
///
/// ## The classification is positional, and deliberately not per-site
///
/// `end_of_input` versus `syntax` is decided ONCE, in `parse_json_reason`,
/// by asking where the cursor stopped: a parse that ran out of bytes is
/// `end_of_input`; one that stopped with bytes still remaining is `syntax`.
/// That rule reproduces every capture above, and it avoids tagging twenty
/// separate `malformed` return sites — each of which would then be a place
/// for the classification to drift.
///
/// `duplicate_field` cannot be positional (the cursor is mid-document
/// either way), so the object parser sets a flag on the reader. It is the
/// only side channel here.
export enum class json_parse_reason : std::uint8_t {
  syntax,          ///< An unexpected byte, with input still remaining. Zig's `SyntaxError`.
  end_of_input,    ///< The document ended mid-value. Zig's `UnexpectedEndOfInput`.
  duplicate_field, ///< An object repeated a key. Zig's `DuplicateField`.
};

/// @brief Maximum container nesting depth.
///
/// A deliberate, documented divergence: the parser below is recursive, so
/// an adversarially-nested document would exhaust the stack. `std.json`'s
/// `Value` parser uses a heap stack and has no such limit, so it would
/// accept a 100,000-deep array this rejects.
///
/// The divergence is a LOUD refusal rather than a silent difference, it is
/// unreachable by any real template (the deepest shipped one nests three
/// levels), and the alternative is a crash. Raise it rather than removing
/// it if a legitimate document ever gets near.
export inline constexpr std::size_t k_max_depth = 512;

/// @brief Decode one complete JSON document.
///
/// Trailing whitespace is allowed; any other trailing byte is
/// `malformed`. Duplicate object keys are `malformed` — see this file's
/// header, item 1.
/// @param text The document bytes.
/// @return The decoded value, or `json_parse_error::malformed`.
export auto parse_json(std::string_view text) -> std::expected<json_value, json_parse_error>;

/// @brief Decode one complete JSON document, reporting WHY a failure failed.
///
/// Identical acceptance to `parse_json` — which is now a thin wrapper that
/// discards the reason — so the two can never disagree about whether a
/// document is valid. See `json_parse_reason`.
/// @param text The document bytes.
/// @return The decoded value, or the reason it could not be decoded.
export auto parse_json_reason(std::string_view text) -> std::expected<json_value, json_parse_reason>;

/// @brief Encode `value` the way
/// `std.json.Stringify(.{ .whitespace = .indent_2 })` does.
///
/// Two-space indent per level, `": "` between key and value, `",\n"`
/// between members, and — the part a plausible re-implementation gets
/// wrong — an EMPTY object or array collapses to `{}` / `[]` with no
/// newline and no inner indent, while a non-empty one always breaks. The
/// result carries NO trailing newline; the caller appends one (see the
/// stdout-terminator contract in `src/lib/engine/local/CMakeLists.txt`).
/// @param value The value to encode.
/// @return The encoded document, without a trailing newline.
export auto stringify_indent2(const json_value& value) -> std::string;

} // namespace planar::json_dom
