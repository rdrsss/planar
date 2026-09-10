/// @file parse.cppm
/// @brief `planar.engine.workbench.parse` — the workbench front-matter
/// parser, and (more importantly) the set of files it REFUSES (plan 996,
/// task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/parse.zig.
///
/// ## The rejection boundary IS the contract
///
/// `planar workbench pull` treats a file whose front matter does not parse
/// as MALFORMED, refuses to apply it, and exits non-zero. So what this
/// parser refuses matters at least as much as what it accepts, and the
/// boundary below was derived by RUNNING the oracle
/// (`workbench lint --path <file> --json`) over a corpus of deliberately
/// malformed files — never by reading parse.zig and never from `--help`.
/// Each line records a probe that was actually executed.
///
/// ACCEPTED (oracle exit 0, or the unrelated `anchor_plan_not_found`
/// warning when the scratch DB had no matching plan):
///
///   - `---` at EOF with no trailing newline (`\n---` terminates the block)
///   - keys with no space after the colon (`entity_kind:task`)
///   - keys indented with SPACES (`  entity_kind: task`)
///   - a blank line inside the front-matter block
///   - single- OR double-quoted values (`'Tech Spec: Auth'`, `"..."`)
///   - a colon with no following space inside a value (`Ratio 3:4`)
///   - a value ENDING in a colon (`Note:`) — render quotes it, parse does not
///     require the quotes
///   - trailing whitespace on any line
///   - unknown keys (silently ignored, mirroring Go)
///   - a duplicate key (last one wins)
///   - free-form `touches:` items — only verifies/cites/derives-from items
///     are validated as entity refs
///   - `entity_id: 1_0` (== 10) and `1__0`; `+5`; leading zeros
///   - a SECOND `---` front-matter block inside the body. The closing
///     delimiter is the FIRST `\n---\n`, so everything after it — nested
///     delimiters included — is body. This is the double-wrap case
///     `artifact update --body @<canonical-workbench-file>` produces; it is
///     silently VALID, which is exactly why it goes unnoticed. Pinned in
///     parse.t.cpp and sync.t.cpp.
///
/// REFUSED (oracle exit 1, `code` as named):
///
///   - no opening `---\n` at byte 0 -> `malformed_frontmatter`
///     (`missing_open_delimiter`). This is also why a UTF-8 BOM and a CRLF
///     file are both rejected outright: neither starts with the exact
///     four bytes `---\n`. Both probed.
///   - no closing `---` -> `missing_close_delimiter`, at the file's LAST line
///   - a TAB anywhere in a line's indentation -> `tab_indentation`
///   - an unquoted value containing `": "` -> `unquoted_colon`
///   - an unquoted value starting `"- "` -> `leading_dash_scalar`
///   - a `- ` list item with no list-introducing key before it -> same
///   - an INDENTED list item (`  - demo`) -> `malformed_yaml`. It is not
///     recognized as a list item (the check is `starts_with("- ")` on the
///     right-trimmed line, so leading spaces defeat it) and then fails the
///     key/value check. Probed; a natural-looking YAML file is rejected here.
///   - a quote-opened value that does not close with the same quote
///     -> `malformed_yaml`
///   - a line with no colon at all, or an empty key -> `malformed_yaml`
///   - a non-base-10 `entity_id` / `anchor_plan_id` / `priority`
///     (`abc`, `0x10`, `_1`, `1_`, `+`, `1 2`, i64 overflow)
///     -> `invalid_integer`
///   - a verifies/cites/derives-from item that is not `<kind>:<positive-id>`
///     -> `invalid_entity_ref` (`task:0` refused; the id must be > 0)
///   - a missing or EMPTY `entity_kind` / `entity_id` / `title` / `status`
///     -> `missing_required_field`. An `entity_id` that parses but is <= 0
///     reports as MISSING, not invalid — probed with `0` and `-5`.
///   - an `entity_kind` outside the six -> `invalid_entity_kind`.
///     `test_scenario` is NOT accepted here even though the sync layer
///     aliases it to `scenario`; probed.
///   - a `status` outside the per-kind set -> `invalid_field_value`
///   - an artifact with a missing or unrecognized `artifact_kind`
///     -> `missing_required_field` / `invalid_field_value`
///
/// ## Diagnostic line numbers are partly SYNTHETIC
///
/// Syntax diagnostics carry the real 1-based line. The four
/// `missing_required_field` diagnostics do NOT: they carry the line the
/// field WOULD occupy in a canonically rendered file — entity_kind 2,
/// entity_id 3, title 5, status 6, artifact_kind 7 — regardless of the
/// file's actual shape. Oracle-confirmed on files where those lines hold
/// something else entirely. Reproduced verbatim per D2.
///
/// ## Memory
///
/// `diagnostic`'s string fields and `parse_result`'s `body` are
/// `std::string_view`s BORROWED from the caller's `content`; `front_matter`
/// owns its strings. The Zig original allocates everything; C++ ownership is
/// idiomatic here rather than transliterated.
module;

export module planar.engine.workbench.parse;

import std;

namespace planar::engine::workbench::parse {

/// @brief A cross-reference of the form `<kind>:<id>`.
export struct entity_ref {
  std::string  kind;   ///< The referenced entity kind, verbatim.
  std::int64_t id = 0; ///< The referenced entity id; always > 0 once parsed.

  /// @brief Memberwise equality.
  /// @return Whether both refs name the same entity.
  auto operator==(const entity_ref&) const -> bool = default;
};

/// @brief The parsed YAML front matter of a workbench Markdown file.
///
/// Field order here is also the RENDER order (see
/// `planar.engine.workbench.render`), which is load-bearing: the rendered
/// bytes must round-trip through this parser unchanged.
export struct front_matter {
  std::string  entity_kind;        ///< One of plan/task/artifact/scenario/decision/question.
  std::int64_t entity_id      = 0; ///< The backing row id; > 0.
  std::int64_t anchor_plan_id = 0; ///< The owning top-level plan, or 0 when absent.
  std::string  title;              ///< Entity title.
  std::string  status;             ///< Entity status, validated per kind.
  std::int64_t priority = 0;       ///< Task priority; 0 when absent.
  std::string  scope;              ///< Free-form scope label; unused by sync.
  std::string  artifact_kind;      ///< Required when `entity_kind == "artifact"`.

  std::vector<std::string> touches;      ///< Repo slugs, unvalidated.
  std::vector<entity_ref>  verifies;     ///< `verifies:` refs.
  std::vector<entity_ref>  cites;        ///< `cites:` refs.
  std::vector<entity_ref>  derives_from; ///< `derives-from:` refs (note the DASH).

  /// @brief Memberwise equality, used by the render/parse round-trip test.
  /// @return Whether every field matches.
  auto operator==(const front_matter&) const -> bool = default;
};

/// @brief A successfully parsed file: its front matter and its body.
export struct parse_result {
  front_matter     frontmatter; ///< The parsed front matter.
  std::string_view body;        ///< Everything after the closing delimiter, borrowed from `content`.
};

/// @brief The four rejection categories. These names are operator-visible:
/// `workbench pull --json` reports them verbatim in `parse_error`, and
/// `workbench lint` maps them onto its `code` field.
export enum class parse_error_kind : std::uint8_t {
  malformed_frontmatter,  ///< Syntax: delimiters, YAML shape, integers, refs.
  missing_required_field, ///< A required field is absent or empty.
  invalid_entity_kind,    ///< `entity_kind` outside the six.
  invalid_field_value,    ///< `status` / `artifact_kind` outside its set.
};

/// @brief The precise rejection, so `lint` can render an actionable message
/// without re-parsing.
export enum class diagnostic_reason : std::uint8_t {
  missing_open_delimiter,
  missing_close_delimiter,
  tab_indentation,
  unquoted_colon,
  leading_dash_scalar,
  malformed_yaml,
  invalid_integer,
  invalid_entity_ref,
  missing_required_field,
  invalid_entity_kind,
  invalid_field_value,
};

/// @brief The first schema or syntax error in a workbench file.
///
/// `field` and `expected` borrow from `content` or from static storage;
/// neither needs freeing. See this file's header on `line` being synthetic
/// for the missing-required-field cases.
export struct diagnostic {
  parse_error_kind  err    = parse_error_kind::malformed_frontmatter; ///< The rejection category.
  diagnostic_reason reason = diagnostic_reason::malformed_yaml;       ///< The precise cause.
  std::size_t       line   = 1;                                       ///< 1-based line; synthetic for missing fields.
  std::string_view  field;                                            ///< The offending field name, when one applies.
  std::string_view  expected;                                         ///< The accepted values, as prose, when one applies.
};

/// @brief The Zig error tag for a rejection category.
///
/// These exact CamelCase bytes reach the operator: zig's
/// `@errorName(parse_err)` lands in `workbench pull --json`'s
/// `malformed_files[].parse_error` and in the verbose text line
/// `MALFORMED: <path> (MalformedFrontmatter)`. Oracle-captured.
/// @param kind The rejection category.
/// @return The Zig error tag.
export auto error_name(parse_error_kind kind) -> std::string_view;

/// @brief Validate a file's front matter without building a `front_matter`.
///
/// The same check gates `parse`, so `lint` and runtime sync accept exactly
/// the same files.
/// @param content The whole file's bytes.
/// @return The first diagnostic, or unset when the file is well-formed.
export auto diagnose(std::string_view content) -> std::optional<diagnostic>;

/// @brief Parse a workbench file's front matter and body.
/// @param content The whole file's bytes; `parse_result::body` borrows from it.
/// @return The parse result, or the rejection category.
export auto parse(std::string_view content) -> std::expected<parse_result, parse_error_kind>;

/// @brief Reproduce `std.fmt.parseInt(i64, s, 10)` exactly.
///
/// Exposed because the accept/reject boundary above depends on it and is
/// separately break-probed. Zig accepts `_` as a digit SEPARATOR, so
/// `entity_id: 1_0` is 10 and `1__0` is also 10 — but a leading or trailing
/// `_` is refused, as is any radix prefix (`0x10` is refused under an
/// explicit base of 10). All five cases oracle-probed.
///
/// A near-identical helper exists at layer 3 (`planar.cmd.planar.args`'s
/// `parse_int64_zig`) for numeric POSITIONALS. It is not reused: D18 forbids
/// a layer-2 -> layer-3 edge outright, and the two call sites are unrelated
/// (an argv token vs. a YAML scalar) even though the grammar coincides.
/// @param raw The scalar text, already trimmed of surrounding whitespace.
/// @return The value, or unset when Zig would return `error.InvalidCharacter`
/// or `error.Overflow`.
export auto parse_int64_zig(std::string_view raw) -> std::optional<std::int64_t>;

/// @brief Strip one layer of matching single or double quotes.
///
/// `'foo'` -> `foo`, `"foo"` -> `foo`, `foo` -> `foo`. Exposed for the
/// renderer's round-trip test.
/// @param value The raw scalar.
/// @return The unquoted scalar.
export auto strip_yaml_quotes(std::string_view value) -> std::string_view;

/// @brief The accepted statuses for `kind`, as the prose the oracle prints.
///
/// The string is BOTH the validation set (split on `,`, an `or ` prefix
/// stripped) and the `hint:` text. Keeping one string for both is how the
/// Zig original guarantees the hint can never drift from the check.
/// @param kind The entity kind.
/// @return The prose list, e.g. `"todo, doing, blocked, done, or cancelled"`.
export auto statuses_for_kind(std::string_view kind) -> std::string_view;

/// @brief The accepted artifact kinds, as the prose the oracle prints.
/// @return The prose list.
export auto artifact_kinds() -> std::string_view;

/// @brief True when `kind` is one of the six projected entity kinds.
///
/// `test_scenario` is NOT one of them at this layer, even though the sync
/// layer aliases it to `scenario`; oracle-probed.
/// @param kind The candidate kind.
/// @return Whether the kind is accepted.
export auto is_entity_kind(std::string_view kind) -> bool;

} // namespace planar::engine::workbench::parse
