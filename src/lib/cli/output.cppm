/// @file output.cppm
/// @brief `planar.cli.output` — uniform text + JSON emission for handler
/// results, mirroring zig/src/cmd/planar/output.zig (task
/// cpp-cli-output-logging).
///
/// The Zig side calls `emit(ctx, mod, value, .{ .json = args.json })` and
/// picks the wire: `json = false` calls `mod.renderText`; `json = true`
/// calls `std.json.Stringify.value`, which serializes a struct's fields in
/// DECLARATION ORDER with no inserted whitespace, and always writes an
/// optional/null field as the literal `null` — the key is never omitted
/// (verified against captured oracle output — see output.t.cpp's header
/// comment for the exact captures, e.g. `plan list --json`'s
/// `"summary":null`). D8 makes Glaze this port's serialization layer
/// (Glaze is already vendored and linked — see CMakeLists.txt); Glaze's
/// default reflection matches the declaration-order/compact-output half of
/// that contract, but Glaze's OWN default (`glz::opts{}`) sets
/// `skip_null_members = true` — the opposite of the Zig shape, OMITTING a
/// null field's key entirely. `emit`/`emit_list` below therefore write
/// through `detail::json_opts` (`skip_null_members = false`), not the
/// bare `glz::write_json` convenience wrapper, to reproduce the Zig
/// oracle's "always-present key" contract. This divergence was caught by
/// this module's own break-probe tests (output.t.cpp) failing against the
/// captured oracle shape before the override was added — see this file's
/// git history / the coder's work-complete report for that discovery.
///
/// `json_opts` is NOT the single override point this comment used to claim
/// (M2 boundary review, plan 996 task 6066, finding B1): Glaze's OWN escape
/// table (vendor/glaze/a4a7/include/glaze/util/parse.hpp:164-186) only
/// covers 7 control characters (`\b \t \n \f \r \" \\`); every other byte
/// in 0x00-0x1F is written RAW by default (Glaze's own comment says so —
/// core/opts.hpp:171: "The default behavior does not escape these
/// characters"), which is INVALID JSON per RFC 8259 §7. Zig's
/// `std.json.Stringify.value` escapes all of 0x00-0x1F unconditionally.
/// Fixing this needs a SECOND override, `escape_control_characters`, which
/// is `requires`-detected rather than a plain field
/// (`check_escape_control_characters`, opts.hpp:533-541: `if constexpr
/// (requires { Opts.escape_control_characters; })`) — a bare
/// `glz::opts{.escape_control_characters = true}` does not compile (`glz::
/// opts` declares no such member; opts.hpp:169 lists it under "Add these
/// fields to a custom options struct if you want to use them"). `json_opts`
/// below is therefore a distinct TYPE (`json_opts_t`) inheriting `glz::
/// opts` and adding the field, the same pattern Glaze's own
/// `opt_true<..., escape_control_characters_opt_tag>` builds at
/// opts.hpp:1080-1084.
///
/// Even with that option, Glaze does not reach byte parity with the Zig
/// oracle: for the 0x00-0x1F bytes that have no short escape, Glaze writes
/// `\uXXXX` with UPPERCASE hex digits (json/write.hpp:811,
/// `"0123456789ABCDEF"`), unconditionally — there is no Glaze option for
/// hex case. Zig's `outputUnicodeEscape` (json/Stringify.zig:636-654) uses
/// `printInt(..., .lower, ...)`, i.e. lowercase. `detail::
/// lowercase_control_escapes` below closes that gap with a second pass over
/// Glaze's own (already-valid) output: a JSON-string-aware walk that
/// lowercases exactly the hex digits of a genuine `\u00XX` escape and
/// leaves every other byte untouched, so the combined result is
/// byte-for-byte what the Zig oracle emits (see that function's own doc
/// comment for why the walk cannot misfire on user content that happens to
/// contain the literal text `A`).
///
/// Every field in a struct passed through `emit`'s JSON path is exported
/// verbatim by name — house style elsewhere uses a trailing underscore on
/// encapsulated implementation state (e.g. db_error::code_), but a type
/// that only exists to be serialized as wire JSON is a plain data shape
/// and keeps bare field names so the JSON keys read the same as the C++
/// member names (no glz::meta rename table needed for the common case).
module;

// Glaze is not a C++ module (traditional #include, matching
// src/lib/core/vendor_probe.cpp's precedent and db.cpp's pattern for the
// vendored SQLite amalgamation) — confined to this interface unit's
// global module fragment. `emit`/`emit_list` are templates, so the
// definitions (and therefore this #include's declarations) must be
// reachable from the module purview for downstream instantiation; that is
// the deliberate trade of putting a generic JSON-emission helper in a
// module interface at all.
#include <glaze/glaze.hpp>

export module planar.cli.output;

import std;

namespace planar::cli {

/// @brief Which wire format `emit`/`emit_list` should write.
export enum class output_format {
  text,
  json,
};

namespace detail {

/// @brief The options TYPE `emit`/`emit_list` write through. Two
/// independent overrides on top of Glaze's defaults, both required for
/// this module's Zig-parity contract (see this file's header comment):
///   - `skip_null_members = false` — Glaze's own default OMITS an
///     absent/`std::nullopt` field's key entirely; the Zig oracle
///     (`std.json.Stringify.value`) always writes the key, `null`
///     included (verified in output.t.cpp's captures — `"summary":null`,
///     `"parent_plan_id":null` are present keys, not omitted ones).
///   - `escape_control_characters = true` — Glaze's own default leaves
///     0x00-0x1F control characters outside its 7-entry short-escape
///     table RAW in the output, which is invalid JSON. This field is
///     `requires`-detected (opts.hpp:533-541), so it can only take effect
///     on a TYPE that declares it, not a `glz::opts{...}` value — hence
///     `json_opts_t` inherits `glz::opts` instead of being one.
struct json_opts_t : glz::opts {
  bool skip_null_members         = false; ///< Always write a null/absent field's key (Zig-parity override).
  bool escape_control_characters = true;  ///< Escape all 0x00-0x1F, not just Glaze's 7-entry table (valid-JSON override).
};

/// @brief The single override point `emit`/`emit_list` write through so
/// every caller gets the Zig-parity shape without having to remember the
/// override itself. See `json_opts_t`'s doc comment for what the two
/// fields fix, and this file's header comment for the residual hex-case
/// gap `lowercase_control_escapes` below closes.
inline constexpr json_opts_t json_opts{};

/// @brief Second pass over Glaze's `escape_control_characters` output,
/// closing the one gap that option alone cannot: Glaze always renders a
/// control-character `\uXXXX` escape with UPPERCASE hex digits
/// (json/write.hpp:811/922, `"0123456789ABCDEF"`, no option to change
/// it); Zig's `outputUnicodeEscape` (json/Stringify.zig:636-654) always
/// renders lowercase. Since Glaze only ever emits `\u00XX` for a genuine
/// 0x00-0x1F control-character escape (there is no other `\u` producer in
/// this option set — non-ASCII bytes pass through raw, not through a
/// unicode escape), lowercasing the two hex digits of every SUCH sequence
/// reaches byte parity.
///
/// The walk is JSON-string-aware (tracks whether we are inside a string
/// literal, and treats a backslash as always consuming exactly the next
/// character as its escape partner) specifically so it cannot misfire on
/// user string content that happens to contain the literal 6 bytes
/// backslash-u-0-0-4-1 as ordinary text: Glaze itself escapes that
/// content's backslash to `\\`, producing `\\u0041` in the wire text, and walking
/// character-by-character consumes that doubled backslash as its OWN
/// two-character escape pair before the scan ever reaches the `u` — so
/// the `u` is seen as an ordinary (non-escape-introducing) character, not
/// mistaken for the start of a new escape sequence.
/// @param text The already Glaze-serialized, already-valid JSON text to
/// normalize in place.
inline auto lowercase_control_escapes(std::string& text) -> void {
  bool in_string = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char const c = text[i];
    if (!in_string) {
      if (c == '"') {
        in_string = true;
      }
      continue;
    }
    if (c == '"') {
      in_string = false;
      continue;
    }
    if (c != '\\') {
      continue;
    }
    // `c` is an unescaped backslash starting an escape sequence; it
    // always consumes exactly one more character (or, for `\u`, four
    // more hex digits) as its partner.
    if (i + 1 >= text.size()) {
      break;
    }
    char const next = text[i + 1];
    if (next == 'u' && i + 5 < text.size()) {
      for (std::size_t k = i + 2; k <= i + 5; ++k) {
        char& h = text[k];
        if (h >= 'A' && h <= 'F') {
          h = static_cast<char>(h - 'A' + 'a');
        }
      }
      i += 5; // skip the full \uXXXX (backslash + 'u' + 4 hex digits).
    } else {
      i += 1; // skip the single escaped character (\n, \", \\, ...).
    }
  }
}

} // namespace detail

/// @brief Emit `value` to `out` in the format `fmt` selects. `text_fn` is
/// the single-value text renderer (the C++ analog of the Zig engine
/// module's `renderText(value, writer)`); it is only invoked on the text
/// path, so a JSON-only caller can pass a renderer that would fail to
/// compile for a type with no natural text form and still use the JSON
/// path unconditionally (the template only instantiates the branch it
/// takes... in practice both branches ARE instantiated together here
/// since `if` is not `if constexpr`; callers with no text form should
/// gate at the call site, mirroring how the Zig side requires every
/// emitted module to declare `renderText` regardless of whether a given
/// invocation asked for `--json`).
/// @param value The value to serialize (must be Glaze-reflectable for the
/// JSON path).
/// @param fmt Which wire format to write.
/// @param text_fn Invoked as `text_fn(value, out)` on the text path.
/// @param out The stream to write to.
export template <class T, class TextFn>
auto emit(T const& value, output_format fmt, TextFn&& text_fn, std::ostream& out) -> void {
  if (fmt == output_format::json) {
    auto result = glz::write<detail::json_opts>(value);
    if (result) {
      detail::lowercase_control_escapes(*result);
      out << *result << '\n';
    } else {
      // Fail loud rather than silently omitting output — a Glaze
      // reflection/encode failure on a first-party type is a defect in
      // the type, not a runtime condition callers should have to check
      // for on every call site.
      out << "{\"error\":\"json serialization failed\"}\n";
    }
  } else {
    text_fn(value, out);
  }
}

/// @brief Emit a range of values as a JSON array (list verbs) or via
/// `text_fn` (the C++ analog of the Zig engine module's
/// `renderListText(values, writer)`).
/// @param values The range of values to serialize.
/// @param fmt Which wire format to write.
/// @param text_fn Invoked as `text_fn(values, out)` on the text path.
/// @param out The stream to write to.
export template <class Range, class TextFn>
auto emit_list(Range const& values, output_format fmt, TextFn&& text_fn, std::ostream& out) -> void {
  if (fmt == output_format::json) {
    auto result = glz::write<detail::json_opts>(values);
    if (result) {
      detail::lowercase_control_escapes(*result);
      out << *result << '\n';
    } else {
      out << "[]\n";
    }
  } else {
    text_fn(values, out);
  }
}

} // namespace planar::cli
