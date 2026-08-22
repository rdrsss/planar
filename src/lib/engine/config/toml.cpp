/// @file toml.cpp
/// @brief Implementation of `planar.engine.config.toml` (see toml.cppm).

module;

#include <glaze/json/generic.hpp>
#include <glaze/toml.hpp>

module planar.engine.config.toml;

import std;

namespace planar::engine::config {

namespace {

/// @brief Recursively flatten a parsed `glz::generic_i64` node into `out`,
/// joining nested object keys with '.'. `prefix` is the dotted path
/// accumulated so far ("" at the document root).
auto flatten(const glz::generic_i64& node, const std::string& prefix, toml_map& out) -> std::expected<void, toml_error> {
  if (node.is_object()) {
    for (const auto& [key, child] : node.get_object()) {
      const std::string full_key = prefix.empty() ? key : std::format("{}.{}", prefix, key);
      if (auto flattened = flatten(child, full_key, out); !flattened) {
        return flattened;
      }
    }
    return {};
  }

  if (node.is_string()) {
    out[prefix] = toml_value{.kind_ = toml_value::kind::string, .string_ = node.get_string()};
    return {};
  }
  if (node.is_boolean()) {
    out[prefix] = toml_value{.kind_ = toml_value::kind::boolean, .bool_ = node.get_boolean()};
    return {};
  }
  if (node.is_int64()) {
    out[prefix] = toml_value{.kind_ = toml_value::kind::integer, .int_ = node.get<std::int64_t>()};
    return {};
  }
  if (node.is_array()) {
    std::vector<std::string> items;
    items.reserve(node.get_array().size());
    for (const auto& elem : node.get_array()) {
      if (!elem.is_string()) {
        // Only string arrays are supported (mirrors zig's parse.zig
        // restriction) — the only array-typed default key
        // (external.github-projects.parent_field_names) is a string
        // array; anything else is out of this config schema's shape.
        return std::unexpected(toml_error::parse_failed);
      }
      items.push_back(elem.get_string());
    }
    out[prefix] = toml_value{.kind_ = toml_value::kind::array, .array_ = std::move(items)};
    return {};
  }

  // Float (is_double() without is_int64()) and null have no counterpart in
  // Planar's config schema — mirrors zig's parse.zig, which rejects both
  // (float values are not supported; TOML has no native null).
  return std::unexpected(toml_error::parse_failed);
}

} // namespace

auto parse_toml(std::string_view content) -> std::expected<toml_map, toml_error> {
  // Glaze's own top-level `read<Opts>` guard rejects a zero-length buffer
  // outright (`error_code::no_read_input`) for EVERY target type, TOML
  // included — short-circuit before ever calling into Glaze so "no file
  // content" and "an empty file" both mean "contributes nothing", matching
  // zig's parse.zig ("empty file returns empty map").
  if (content.empty()) {
    return toml_map{};
  }

  // Read into a `std::map<std::string, generic_i64>` — NOT a bare
  // `glz::generic_i64` — for the document root. This is not a stylistic
  // choice: `glz::generic_i64`'s own `glz::meta` unwraps it to its
  // internal `data` variant, so a bare top-level `glz::read_toml(generic_i64&,
  // ...)` dispatches through Glaze's VARIANT reader (toml/read.hpp's
  // `from<TOML, T> requires is_variant<T>`), which decides what a
  // document IS purely from its first non-whitespace/non-comment
  // character. Every real Planar config file's first significant
  // character is `[` (a table header — `defaults.toml` opens with
  // `[defaults]`), and the variant reader's `case '['` branch
  // unconditionally treats a leading `[` as "the whole document is a TOML
  // ARRAY literal" (toml/read.hpp:2692-2707) — it has no lookahead to
  // distinguish `[defaults]\nvendor = "x"` (a table header) from
  // `[1, 2, 3]` (an array value). A bare `glz::generic_i64` therefore
  // silently misparses (or, here, error_code::syntax_errors on) any
  // document starting with a section header. Reading into a genuine
  // `readable_map_t` (a `std::map`) instead routes through the DIFFERENT
  // `from<TOML, T> requires readable_map_t<T>` specialization
  // (toml/read.hpp:2202), which correctly implements table-header/
  // section-path handling for its whole `while` loop. Verified against
  // both code paths with a standalone probe before writing this
  // workaround — see this task's work-complete report for the captured
  // repro. This is a real, narrow gap in Glaze's generic-TOML reading
  // (not a wholesale invalidation of D12: Glaze's TYPED-struct TOML
  // reading, which `docs/toml.md`'s own examples use, has no such
  // problem — only the schema-less `glz::generic`/`generic_i64` path
  // does, and only when read directly at top level).
  std::map<std::string, glz::generic_i64, std::less<>> root;
  const auto                                           ec = glz::read_toml(root, content);
  if (ec) {
    return std::unexpected(toml_error::parse_failed);
  }

  toml_map out;
  for (const auto& [key, child] : root) {
    if (auto flattened = flatten(child, key, out); !flattened) {
      return std::unexpected(flattened.error());
    }
  }
  return out;
}

} // namespace planar::engine::config
