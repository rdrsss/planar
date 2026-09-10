/// @file render.cppm
/// @brief `planar.engine.workbench.render` — `front_matter` + body ->
/// the exact bytes of a workbench Markdown file (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/render.zig.
///
/// ## Terminator contract
///
/// `render` returns a COMPLETE file payload: the caller writes the returned
/// string verbatim and appends nothing. (Per the tree's per-renderer rule —
/// see src/lib/json_text/CMakeLists.txt — this is stated here rather than
/// assumed from a blanket convention.)
///
/// ## The shape, oracle-captured
///
/// Byte-for-byte from a real `workbench push` against a scratch tree:
///
///     ---\n
///     entity_kind: artifact\n
///     entity_id: 1\n
///     anchor_plan_id: 1\n
///     title: 'Tech Spec: Auth'\n
///     status: draft\n
///     artifact_kind: tech_spec\n
///     ---\n
///     \n
///     # Artifact 1: Tech Spec: Auth\n
///     ...
///
/// Field order is FIXED (it is `front_matter`'s declaration order), the
/// three identity fields are always emitted, and every other field is
/// emitted only when non-empty / non-zero. A single `\n` separates the
/// closing delimiter from a non-empty body; an empty body gets nothing.
///
/// ## Quoting
///
/// A scalar is single-quoted when it would otherwise re-parse wrong:
/// empty, starting with a YAML indicator, containing `": "`, or ending in
/// `:`. Note the asymmetry with the parser — `title: Note:` is ACCEPTED by
/// `parse` but is QUOTED by `render`, so a hand-written file is normalized
/// on the next push. Both halves oracle-probed.
///
/// ## Round trip
///
/// `parse(render(fm, body))` reconstructs `fm` and `body` for every
/// front matter this tree produces. That is pinned in render.t.cpp rather
/// than asserted here, and it is the reason field order is not "tidied".
module;

export module planar.engine.workbench.render;

import std;
import planar.engine.workbench.parse;

namespace planar::engine::workbench::render {

/// @brief Render a complete workbench Markdown file.
/// @param fm The front matter to serialize.
/// @param body The Markdown body; may be empty.
/// @return The complete file bytes, terminator included.
export auto render(const parse::front_matter& fm, std::string_view body) -> std::string;

/// @brief True when `value` must be single-quoted to survive a round trip.
///
/// Exposed for the quoting-boundary test; not otherwise part of the
/// bucket's vocabulary.
/// @param value The scalar to inspect.
/// @return Whether the renderer will quote it.
export auto needs_yaml_quote(std::string_view value) -> bool;

} // namespace planar::engine::workbench::render
