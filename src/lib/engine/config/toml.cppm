/// @file toml.cppm
/// @brief `planar.engine.config.toml` — flattens a TOML document into a
/// dotted-path map (tech-spec § "Key mechanisms" / D12: Glaze owns TOML).
///
/// D12 assumption CONFIRMED: `vendor/glaze/a4a7/include/glaze/toml/{read,
/// write}.hpp` ship a full TOML 1.1 reader/writer (`glz::read_toml`,
/// `docs/toml.md`) — no substitute library was needed. One narrow, real
/// gap WAS found and worked around (documented at the call site in
/// toml.cpp): reading directly into a bare `glz::generic_i64` at the
/// document root mis-dispatches through Glaze's variant reader, which
/// treats a document opening with `[section]` as an array literal.
/// Reading into a `std::map<std::string, glz::generic_i64>` instead (a
/// genuine `readable_map_t`) routes through Glaze's correct table-header-
/// aware map reader and has no such problem — Glaze's TYPED-struct TOML
/// reading (`docs/toml.md`'s own examples) never hits this at all; only
/// the schema-less generic path does. Rather than
/// hand-roll a second TOML grammar (the way zig/src/engine/config/parse.zig
/// does, D-engine-pattern notwithstanding), this module reads the document
/// through Glaze's generic JSON-shaped type (`glz::generic_i64` — the
/// `_i64` variant so `introspection.retention_days` round-trips as an exact
/// `std::int64_t`, matching Zig's `parse.Value.int` field) and flattens the
/// resulting nested object into the SAME flat dotted-key contract
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
/// is reported as `toml_error::parse_failed`.
module;

#include <glaze/toml.hpp>

export module planar.engine.config.toml;

import std;

namespace planar::engine::config {

/// @brief Error surface for `parse_toml`.
export enum class toml_error : std::uint8_t {
  parse_failed, ///< Glaze failed to parse the document, or it used an unsupported shape (float, non-string array element).
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
/// (`std::less<>`) so callers that want lexicographic key iteration (e.g. a
/// future `config show --effective`) get it for free, matching the way
/// `effective.zig`'s `sortedKeys` has to explicitly sort a hash map.
export using toml_map = std::map<std::string, toml_value, std::less<>>;

/// @brief Parse `content` as TOML and flatten it into a dotted-path map.
/// @param content The raw TOML document text (embedded defaults, or a
/// user's `~/.planar/config.toml`).
/// @return The flattened map (empty for an empty/comments-only document),
/// or `toml_error::parse_failed`.
export auto parse_toml(std::string_view content) -> std::expected<toml_map, toml_error>;

} // namespace planar::engine::config
