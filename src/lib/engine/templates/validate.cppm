/// @file validate.cppm
/// @brief `planar.engine.templates.validate` — smoke-render a template
/// against a stub context to surface authoring mistakes (plan 996, task
/// 6190).
///
/// Behavior-preserving port (D2) of
/// `zig/src/engine/templates/validate.zig`.
///
/// JSON well-formedness is NOT this module's job — a template that does not
/// parse never reaches here, because `engine_config`'s `load_template`
/// rejects it one level down and the leaf reports "not found" instead. What
/// this checks is DIRECTIVE correctness: every `{{...}}` in every string,
/// exercised for real.
///
/// ## The walk and the smoke render are BOTH kept, and they overlap
///
/// `validate` does two passes over the same tree:
///
///   1. a per-string walk that renders each string INDEPENDENTLY and
///      attributes any failure to that string's dotted JSON path;
///   2. a whole-template `render_template` whose failure is attributed to
///      the pseudo-path `(smoke-render)`.
///
/// On today's renderer these find the same errors, so a broken template
/// reports its failure TWICE — once against the real field and once against
/// `(smoke-render)`. That double-report is oracle-captured, not a bug in
/// this port: a template with five broken fields yields SIX issues and the
/// leaf's summary line says `6 issue(s)`. Collapsing the duplicate would
/// change the count on stderr.
///
/// ## Issue ORDER is document order, not severity
///
/// Issues come out in the order the walk reaches them — object members in
/// insertion order, array elements by index — with `(smoke-render)` always
/// last. Both the text and the JSON leaf output preserve it.

module;

export module planar.engine.templates.validate;

import std;
import planar.json_dom;

namespace planar::engine::templates {

using json_dom::json_value;

/// @brief One problem found while validating a template.
export struct validation_issue {
  /// @brief Dotted path to the offending field, e.g. `fields.summary` or
  /// `labels[0]`. The literal `(smoke-render)` for the whole-template pass.
  ///
  /// A top-level string member's path is its bare key (no leading dot); an
  /// array element under key `k` is `k[0]`. An array at the ROOT of the
  /// template yields `[0]` with no prefix, which is the oracle's shape.
  std::string json_path;
  /// @brief The failure. For the per-field walk this is the bare Zig error
  /// name (`UnknownField`); for the smoke pass it is
  /// `smoke render failed: <name>`. Both spellings are printed verbatim by
  /// the leaf and are pinned contract.
  std::string message;
};

/// @brief Validate every string field of a decoded template.
/// @param fields The decoded template tree.
/// @return Every issue, in document order, `(smoke-render)` last. Empty for
/// a clean template.
export auto validate_template(const json_value& fields) -> std::vector<validation_issue>;

} // namespace planar::engine::templates
