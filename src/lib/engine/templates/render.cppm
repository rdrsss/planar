/// @file render.cppm
/// @brief `planar.engine.templates.render` — `{{...}}` substitution over a
/// decoded template tree (plan 996, task 6190).
///
/// Behavior-preserving port (D2) of `zig/src/engine/templates/render.zig`.
///
/// ## The supported directive surface, and why it refuses the rest
///
/// This is NOT a `text/template` implementation. It is the strict subset
/// the shipped templates exercise:
///
///   `{{.A.B}}`                     field reference
///   `{{.}}`                        the current item, inside a `range` body
///   `{{range .Touches}}…{{end}}`   iterate repo slugs
///   `{{range .Children}}…{{end}}`  iterate child plans/tasks
///   `{{if .Field}}…{{end}}`        emit body when the field is truthy
///
/// Everything else — `else`, `with`, pipelines, custom funcs, `range` over
/// anything but the two named slices — is `unsupported_directive`. Failing
/// loudly is the intent: a silently-ignored directive renders a
/// plausible-looking payload that then gets PUSHED to Jira or GitHub.
///
/// **`{{else}}` is the one place that intent does not hold**, and the gap
/// is reproduced rather than closed. `{{else}}` is not counted while
/// scanning for the matching `{{end}}`, so:
///
///   `{{if <falsy>}}a{{else}}b{{end}}`  -> `""`, exit 0. The `b` branch is
///                                        SILENTLY DROPPED.
///   `{{if <truthy>}}a{{else}}b{{end}}` -> `unsupported_directive`, exit 1.
///
/// Both captured from the built oracle. An operator who writes an `else`
/// therefore gets a silently wrong payload half the time and a hard
/// failure the other half. Needs its own task against the oracle; see
/// `src/lib/engine/templates/CMakeLists.txt`, which lists it with the
/// other reproduced defects.
///
/// ## Error names are the wire format
///
/// `templates validate` prints these enumerators BY NAME
/// (`ISSUE: <path> [body]: UnsupportedDirective`), because the Zig handler
/// formats `@errorName(e)` straight into its output. So `error_name()`
/// below returns the ZIG spelling — PascalCase, no underscores — and the
/// C++ enumerator names are irrelevant to the contract. Renaming an
/// enumerator is free; changing what `error_name` returns is a parity
/// break. Oracle-captured, all five.
///
/// ## The 128-byte truthiness budget IS the oracle, and it IS a defect
///
/// `{{if .X}}` used to resolve `.X` into a 128-BYTE STACK BUFFER purely to
/// ask "is this non-empty?", and a value that did not fit failed the ENTIRE
/// render. Against the built oracle:
///
///   task body of 128 bytes -> exit 0, `{"x": "HAS-BODY"}`
///   task body of 129 bytes -> exit 1, `error: rendering template: OutOfMemory`
///
/// So `planar templates render` — and every `ext propagate` path sharing
/// this renderer — refused outright for any task whose body exceeded 128
/// bytes under an `{{if .Task.Body}}` guard, which the shipped
/// `templates/defaults/github-issues/issue.json` uses.
///
/// REMOVED AT TASK 6210 under decision 1067, which ended D2's bug-for-bug
/// rule once the oracle was deleted and named this row in its FIX set.
/// Truthiness asks whether the rendered text is non-empty; length was never
/// part of the rule, and the ceiling was an artifact of how the oracle
/// resolved the value (a `FixedBufferAllocator` over `var tmp: [128]u8`).
///
/// `k_truthy_budget` survives as a NAMED HISTORICAL CONSTANT only — nothing
/// reads it. It is kept because the render tests pin the exact byte the old
/// ceiling rejected, and a reader finding 129 in a test deserves to find
/// what 129 meant.

module;

export module planar.engine.templates.render;

import std;
import planar.engine.templates.context;
import planar.json_dom;

namespace planar::engine::templates {

// The DOM lives at LAYER 1 (`planar.json_dom`) rather than in this bucket:
// the RESOLUTION half of the template plane needs the same parser and is a
// different layer-2 bucket, which cannot import this one. See
// src/lib/json_dom/CMakeLists.txt for the differential that forced it.
using json_dom::json_value;

/// @brief Why a render failed. See this file's header: the NAMES these map
/// to are the wire format, not these enumerators.
export enum class render_error : std::uint8_t {
  unsupported_directive, ///< A directive outside the supported subset. Zig: `UnsupportedDirective`.
  unknown_field,         ///< A `{{.Path}}` naming no reachable field. Zig: `UnknownField`.
  unclosed_directive,    ///< A `{{` with no matching `}}`, or a `range`/`if` with no `{{end}}`. Zig: `UnclosedDirective`.
  unexpected_end,        ///< Input exhausted while a stop keyword was still expected. Zig: `UnexpectedEnd`.
  out_of_memory,         ///< The 128-byte truthiness budget was exceeded. Zig: `OutOfMemory`.
};

/// @brief The exact `@errorName` spelling the Zig oracle prints for `e`.
///
/// `templates validate` writes this verbatim into both its text and its
/// JSON output, so these five strings are pinned contract.
/// @param e The error.
/// @return The Zig error name.
export auto error_name(render_error e) -> std::string_view;

/// @brief The truthiness-evaluation budget, in bytes.
///
/// HISTORICAL. Nothing reads this. zig's `evalTruthy` backed
/// `resolveReference` with `var tmp: [128]u8` + a `FixedBufferAllocator`,
/// so resolving a longer value to decide `{{if}}` failed the whole render —
/// verified against the oracle at exactly 128 (pass) and 129 (fail), and
/// reproduced here until task 6210 removed it under decision 1067. Retained
/// so the number appearing in the render tests has a definition to point at.
/// Do NOT wire it back into `eval_truthy`.
export inline constexpr std::size_t k_truthy_budget = 128;

/// @brief Substitute every `{{...}}` directive in one string.
///
/// This is the unit `templates validate` exercises per JSON field, which
/// is why it is exported rather than being an implementation detail of
/// `render_template`.
/// @param src The raw string value from the template.
/// @param ctx The data to render against.
/// @return The substituted text, or the first error encountered.
export auto exec_string(std::string_view src, const render_context& ctx) -> std::expected<std::string, render_error>;

/// @brief Render a whole decoded template tree.
///
/// Every STRING leaf is substituted; numbers, booleans, nulls, and the
/// container shapes themselves pass through untouched. Object key ORDER is
/// preserved (see `json_dom.cppm` for why that is load-bearing) and keys
/// themselves are NOT substituted — a `{{...}}` in a key renders literally,
/// matching the oracle, which only ever walks values.
/// @param fields The decoded template.
/// @param ctx The data to render against.
/// @return The substituted tree, or the first error encountered.
export auto render_template(const json_value& fields, const render_context& ctx) -> std::expected<json_value, render_error>;

} // namespace planar::engine::templates
