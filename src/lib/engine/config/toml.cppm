/// @file toml.cppm
/// @brief `planar.engine.config.toml` — flattens a TOML document into a
/// dotted-path map (tech-spec § "Key mechanisms" / D12: Glaze owns TOML).
///
/// D12 assumption CONFIRMED: `vendor/glaze/a4a7/include/glaze/toml/{read,
/// write}.hpp` ship a full TOML 1.1 reader/writer (`glz::read_toml`,
/// `docs/toml.md`) — no substitute library was needed. Two narrow, real
/// gaps WERE found; both are handled here rather than by patching the
/// pinned vendor archive, and both are documented at their call sites in
/// toml.cpp:
///
///   1. Reading directly into a bare `glz::generic_i64` at the document
///      root mis-dispatches through Glaze's variant reader, which treats a
///      document opening with `[section]` as an array literal (task 6076).
///      Reading into a `std::map<std::string, glz::generic_i64>` instead —
///      a genuine `readable_map_t` — routes through Glaze's correct
///      table-header-aware map reader. Glaze's TYPED-struct TOML reading
///      (`docs/toml.md`'s own examples) never hits this; only the
///      schema-less generic path does.
///   2. Glaze's `ensure_map_path` rejects a table header whose path was
///      already materialized by a DEEPER header ("Re-defining an
///      already-defined table is invalid", toml/read.hpp:2010-2012), so
///      `[external.jira.status]` followed by `[external.jira]` fails the
///      whole document. That ordering is legal TOML and the Zig oracle
///      accepts it (task 6082). `normalize_sections` (toml.cpp) rewrites
///      the document so every table header is opened exactly once and
///      shallower headers always precede the deeper headers that extend
///      them, before Glaze ever sees it. This is the "validation layer on
///      top of Glaze" the tech spec's own D12 note anticipated, not a
///      second TOML grammar.
///
/// Rather than hand-roll a second TOML grammar (the way
/// zig/src/engine/config/parse.zig does, D-engine-pattern notwithstanding),
/// this module reads the document through Glaze's generic JSON-shaped type
/// (`glz::generic_i64` — the `_i64` variant so
/// `introspection.retention_days` round-trips as an exact `std::int64_t`,
/// matching Zig's `parse.Value.int` field) and flattens the resulting
/// nested object into the SAME flat dotted-key contract
/// `zig/src/engine/config/parse.zig`'s hand-rolled parser produces (e.g.
/// `[external.jira.status]` + `todo = "To Do"` → key
/// `"external.jira.status.todo"`). `effective.cppm`'s resolve() consumes
/// that flat map exactly the way `effective.zig`'s `resolve()` consumes
/// `parse.zig`'s flat map — same precedence logic, different TOML front
/// end.
///
/// A TOML quoted table-header segment (`[external."github-issues"]`)
/// flattens to the SAME bare dotted key as an unquoted one
/// (`"external.github-issues"`) — Glaze's object keys never retain the
/// source quoting, matching zig's own `parseKeySegment` (which also
/// unquotes before joining with '.'). This module's array support is
/// intentionally the same restricted subset zig's parser accepts: arrays
/// of strings only (the only array-typed default key,
/// `external.github-projects.parent_field_names`, is a string array); an
/// array containing a non-string element, or a float value anywhere in the
/// document (TOML floats have no counterpart in Planar's config schema),
/// is rejected.
///
/// ## Known, deliberate divergences from the Zig oracle
///
/// Both directions were measured against `zig/zig-out/bin/planar config
/// validate` in a pinned scratch arena (task 6082):
///
///   - MORE permissive than Zig, because Glaze is a full TOML parser and
///     `parse.zig` is a restricted subset: inline tables (`opts = {a = 1}`),
///     multi-line strings (`x = """hi"""`), and numeric underscores
///     (`x = 1_000`) all parse here and are hard errors in Zig. These are
///     legal TOML; accepting them is a superset, so no config an operator
///     could previously load stops loading.
///   - STRICTER than Zig on DUPLICATE KEYS: `vendor = "a"` twice in one
///     table is an error here and last-wins in `parse.zig:359-367`. The
///     TOML specification makes duplicate keys an error, so Glaze is
///     conformant and Zig is not. Left strict deliberately. (Duplicate
///     TABLE HEADERS — `[defaults]` appearing twice with disjoint keys —
///     are ACCEPTED, because `normalize_sections` merges them into one
///     section before Glaze sees them; that matches Zig.)
module;

#include <glaze/toml.hpp>

export module planar.engine.config.toml;

import std;

namespace planar::engine::config {

/// @brief A TOML parse failure, carrying the source location the way zig's
/// `parse.ParseError` does so `config validate` can print
/// `error: line {line}: col {column}: TOML parse error: {message}`
/// (zig/src/cmd/planar/handlers/config/validate.zig:38-55).
export struct toml_error {
  /// @brief 1-based source line, or 0 when the failure could not be
  /// attributed to a line (a Glaze error whose byte offset is past the end
  /// of the buffer, or a semantic rejection whose key is not locatable).
  std::uint32_t line = 0;
  /// @brief 1-based source column, or 0 when unknown (see `line`).
  std::uint32_t column = 0;
  /// @brief Human-readable cause. Semantic rejections this module raises
  /// itself reuse zig's exact wording ("float values are not supported");
  /// syntax errors carry Glaze's own `error_code` name, since the two
  /// parsers do not share a diagnostic vocabulary.
  std::string message;
};

/// @brief A single flattened TOML leaf value. Mirrors zig's
/// `parse.Value` union (`string` / `int` / `bool` / `array` of strings).
export struct toml_value {
  /// @brief Which member of this value is populated.
  enum class kind : std::uint8_t { string, integer, boolean, array };

  kind                     kind_ = kind::string; ///< The active member.
  std::string              string_;              ///< Populated when `kind_ == kind::string`.
  std::int64_t             int_  = 0;            ///< Populated when `kind_ == kind::integer`.
  bool                     bool_ = false;        ///< Populated when `kind_ == kind::boolean`.
  std::vector<std::string> array_;               ///< Populated when `kind_ == kind::array`.
};

/// @brief A flattened TOML document: dotted-path key → leaf value. Ordered
/// (`std::less<>`) so callers that want lexicographic key iteration (e.g.
/// `config show --effective`) get it for free, matching the way
/// `effective.zig`'s `sortedKeys` has to explicitly sort a hash map.
export using toml_map = std::map<std::string, toml_value, std::less<>>;

/// @brief Parse `content` as TOML and flatten it into a dotted-path map.
/// @param content The raw TOML document text (embedded defaults, or a
/// user's `~/.planar/config.toml`).
/// @return The flattened map (empty for an empty/comments-only document),
/// or a located `toml_error`.
export auto parse_toml(std::string_view content) -> std::expected<toml_map, toml_error>;

/// @brief Rewrite `content` so that every table header appears exactly once
/// and no header precedes a shallower header that it extends.
///
/// Exported for testing only: this is the workaround for Glaze's
/// `ensure_map_path` table-redefinition rejection (task 6082) and for
/// duplicate table headers, and a Glaze bump that fixed either would make
/// the corresponding pins in toml.t.cpp redundant. Section bodies are
/// copied verbatim, so a caller can map a normalized line number back to
/// its source line through the returned origin table.
/// @param content The raw TOML document text.
/// @return The rewritten document, and a table whose i-th entry is the
/// 1-based source line that produced normalized line i+1.
export struct normalized_document {
  std::string                text;        ///< The rewritten document.
  std::vector<std::uint32_t> line_origin; ///< Normalized line (0-based index) → 1-based source line.
};

/// @brief See `normalized_document`.
/// @param content The raw TOML document text.
/// @return The rewritten document plus its line-origin table.
export auto normalize_sections(std::string_view content) -> normalized_document;

} // namespace planar::engine::config
