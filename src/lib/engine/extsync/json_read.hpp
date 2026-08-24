// @file json_read.hpp
// @brief The three read-side JSON accessors both adapter implementations in
// this bucket need (plan 996, task 6041).
//
// A PRIVATE header rather than an exported module, because its signatures
// name `glz::generic` — Glaze is not a module, so exporting these would put a
// `#include <glaze/...>` into a module interface every consumer then inherits.
// Both `jira.cpp` and `github.cpp` are in the SAME target, so a shared header
// between them creates no dependency edge (unlike the layer-crossing case
// `support.cppm` next door documents).
//
// These are ports of the `getObjectString` helper both Zig adapter files
// declare identically (jira.zig:305-309, github.zig:854-858) plus the two
// container guards its call sites always pair with. The behavior that matters
// is what they return for a WRONG-TYPED value: the Zig version checks
// `value != .string` and returns null, so a JSON `null` (which Jira sends for
// an unset `description`, `assignee`, `priority` and `duedate`) reads as
// "absent", not as the string "null" and not as a parse failure. Every guard
// below preserves that.
#pragma once

#include <glaze/glaze.hpp>

namespace planar::engine::extsync::json_read {

/// @brief Read `key` from `obj` as a string.
/// @param obj The value to read from; need not be an object.
/// @return The string, or unset when `obj` is not an object, `key` is absent,
/// or the value is not a string (JSON `null` included).
inline auto string_field(const glz::generic& obj, std::string_view key) -> std::optional<std::string> {
  if (!obj.is_object() || !obj.contains(key)) {
    return std::nullopt;
  }
  auto const& value = obj.at(key);
  if (!value.is_string()) {
    return std::nullopt;
  }
  return value.get<std::string>();
}

/// @brief Read `key` from `obj` as a nested object.
/// @param obj The value to read from.
/// @param key The field name.
/// @return A pointer to the nested object, or null when absent or not an
/// object. The pointer borrows from `obj`.
inline auto object_field(const glz::generic& obj, std::string_view key) -> const glz::generic* {
  if (!obj.is_object() || !obj.contains(key)) {
    return nullptr;
  }
  auto const& value = obj.at(key);
  return value.is_object() ? &value : nullptr;
}

/// @brief Read `key` from `obj` as an array.
/// @param obj The value to read from.
/// @param key The field name.
/// @return A pointer to the array, or null when absent or not an array. The
/// pointer borrows from `obj`.
inline auto array_field(const glz::generic& obj, std::string_view key) -> const glz::generic::array_t* {
  if (!obj.is_object() || !obj.contains(key)) {
    return nullptr;
  }
  auto const& value = obj.at(key);
  return value.is_array() ? &value.get<glz::generic::array_t>() : nullptr;
}

} // namespace planar::engine::extsync::json_read
