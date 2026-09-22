/// @file config.cpp
/// @brief `find_config_value`: decode `planar config show --json` with Glaze
/// (plan 1033 M0, task 6485). `read_planar_config`, which spawns the sibling
/// `planar`, stays in host.cpp beside the file-local process seam it uses.

module;

#include <glaze/glaze.hpp>

module planar.engine_execute;

import std;

namespace planar::engine::execute {

// A named namespace, not an anonymous one: Glaze's reflection needs the
// decoded type to have linkage (same shape as `engine/local`'s `wire`).
namespace wire {

/// @brief One line of `planar config show --json`.
struct config_line {
  std::string key;        ///< The dotted key.
  std::string value;      ///< The resolved value.
  std::string provenance; ///< Where the value came from.
};

} // namespace wire

auto find_config_value(std::string_view ndjson, std::string_view key) -> std::optional<config_value> {
  // One object per line. Unknown fields are ignored so a newer `planar` that
  // adds one does not make this binary's config read fail.
  constexpr glz::opts k_opts{.error_on_unknown_keys = false};
  while (!ndjson.empty()) {
    auto const nl   = ndjson.find('\n');
    auto const line = ndjson.substr(0, nl);
    ndjson.remove_prefix(nl == std::string_view::npos ? ndjson.size() : nl + 1);
    if (line.empty()) {
      continue;
    }
    wire::config_line parsed{};
    if (glz::read<k_opts>(parsed, line) || parsed.key != key) {
      continue;
    }
    return config_value{.value = std::move(parsed.value), .provenance = std::move(parsed.provenance)};
  }
  return std::nullopt;
}

} // namespace planar::engine::execute
