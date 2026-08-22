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

/// @brief Glaze's own default (`glz::opts{}`) sets `skip_null_members =
/// true` — an absent/`std::nullopt` field is OMITTED from the object
/// entirely. The Zig oracle (`std.json.Stringify.value`) does the
/// opposite: every declared field is always written, `null` included
/// (verified in output.t.cpp's captures — `"summary":null`,
/// `"parent_plan_id":null` are present keys, not omitted ones). This
/// options value is `emit`/`emit_list`'s single override point so every
/// caller gets the Zig shape without having to remember the override
/// itself.
inline constexpr glz::opts json_opts{.skip_null_members = false};

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
      out << *result << '\n';
    } else {
      out << "[]\n";
    }
  } else {
    text_fn(values, out);
  }
}

} // namespace planar::cli
