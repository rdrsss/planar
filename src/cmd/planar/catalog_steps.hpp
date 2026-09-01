// @file catalog_steps.hpp
// @brief Catalog-to-argv conversion for the state-differential lane.
//
// The state lane must never manufacture a command name.  Its generated
// invocations are derived from the two installed `planar schema` documents,
// and are admitted only when the catalog says no argument is required.  The
// ordered, data-dependent part of the lane stays in statediff.t.cpp: a
// catalog cannot know which id a preceding mutation created.
#pragma once

#include <glaze/glaze.hpp>

namespace planar::cmd::state_catalog {

/// @brief One generated invocation and its unambiguous catalog identity.
struct step {
  std::string              key;  ///< JSON serialization of the path tokens.
  std::vector<std::string> args; ///< Exact argv elements, excluding argv[0].
};

namespace detail {

struct leaf {
  std::vector<std::string> path;
  bool                     has_json           = false;
  bool                     has_required_input = false;
};

inline auto path_key(std::span<const std::string> path, std::string& error) -> std::optional<std::string> {
  auto encoded = glz::write_json(path);
  if (!encoded) {
    error = "serializing catalog path failed";
    return std::nullopt;
  }
  return *encoded;
}

inline auto required_inputs(const glz::generic& values, std::string_view label, bool& required, bool& has_json,
                            std::string& error) -> bool {
  if (!values.is_array()) {
    error = std::format("catalog {} is not an array", label);
    return false;
  }
  for (auto const& value : values.get<glz::generic::array_t>()) {
    if (!value.is_object() || !value.contains("required") || !value.at("required").is_boolean()) {
      error = std::format("catalog {} entry has no boolean required field", label);
      return false;
    }
    required = required || value.at("required").get<bool>();
    if (label == "flags") {
      if (!value.contains("long") || !value.at("long").is_string()) {
        error = "catalog flag has no string long field";
        return false;
      }
      has_json = has_json || value.at("long").get<std::string>() == "--json";
    }
  }
  return true;
}

inline auto leaves(std::string_view text, std::string& error) -> std::optional<std::map<std::string, leaf, std::less<>>> {
  auto parsed = glz::read_json<glz::generic>(text);
  if (!parsed || !parsed->is_object() || !parsed->contains("commands") || !parsed->at("commands").is_array()) {
    error = "catalog JSON is malformed or has no commands array";
    return std::nullopt;
  }

  std::map<std::string, leaf, std::less<>> result;
  for (auto const& command : parsed->at("commands").get<glz::generic::array_t>()) {
    if (!command.is_object() || !command.contains("path") || !command.at("path").is_array() || !command.contains("subcommands") ||
        !command.at("subcommands").is_array() || !command.contains("positionals") || !command.contains("flags")) {
      error = "catalog command has an incomplete shape";
      return std::nullopt;
    }
    if (!command.at("subcommands").get<glz::generic::array_t>().empty()) {
      continue;
    }

    leaf parsed_leaf;
    for (auto const& token : command.at("path").get<glz::generic::array_t>()) {
      if (!token.is_string()) {
        error = "catalog path has a non-string token";
        return std::nullopt;
      }
      parsed_leaf.path.push_back(token.get<std::string>());
    }
    if (parsed_leaf.path.empty()) {
      error = "catalog leaf has an empty path";
      return std::nullopt;
    }
    if (!required_inputs(command.at("positionals"), "positionals", parsed_leaf.has_required_input, parsed_leaf.has_json, error) ||
        !required_inputs(command.at("flags"), "flags", parsed_leaf.has_required_input, parsed_leaf.has_json, error)) {
      return std::nullopt;
    }

    auto key = path_key(parsed_leaf.path, error);
    if (!key || !result.emplace(*key, std::move(parsed_leaf)).second) {
      if (error.empty()) {
        error = "catalog declares a duplicate leaf path";
      }
      return std::nullopt;
    }
  }
  return result;
}

inline auto eligible(const leaf& candidate) -> bool {
  // Decision 980 excludes the cockpit from byte-level state parity.
  // `config edit` is the other intentional exception: it is catalog-valid
  // without input but launches $EDITOR, so an automated state lane would
  // hang rather than exercise a deterministic command.  `schema`, `skills`,
  // `version`, `assoc detect`, and `scope suggest` are output-only surfaces
  // (the latter two derive arena-specific paths), so they have no durable
  // state to diff.
  // `workspace init` remains an explicit late-port case while it returns the
  // designated unimplemented refusal. Stateful commands with required input
  // stay in the explicit sequence beside this helper, where their dependency
  // ordering is visible.
  return !candidate.has_required_input && candidate.path != std::vector<std::string>{"explore"} &&
         candidate.path != std::vector<std::string>{"config", "edit"} && candidate.path != std::vector<std::string>{"schema"} &&
         candidate.path != std::vector<std::string>{"skills"} && candidate.path != std::vector<std::string>{"version"} &&
         candidate.path != std::vector<std::string>{"assoc", "detect"} &&
         candidate.path != std::vector<std::string>{"scope", "suggest"} &&
         candidate.path != std::vector<std::string>{"workspace", "init"};
}

} // namespace detail

/// @brief Derive the complete eligible inventory from both schema catalogs.
/// @param cpp_json The C++ binary's `schema` stdout.
/// @param zig_json The Zig oracle's `schema` stdout.
/// @param error Receives a refusal suitable for a test diagnostic.
/// @return Generated argv steps, or unset before either subject binary runs.
inline auto generated_steps(std::string_view cpp_json, std::string_view zig_json, std::string& error)
    -> std::optional<std::vector<step>> {
  error.clear();
  auto cpp = detail::leaves(cpp_json, error);
  if (!cpp) {
    return std::nullopt;
  }
  auto zig = detail::leaves(zig_json, error);
  if (!zig) {
    return std::nullopt;
  }

  std::set<std::string, std::less<>> cpp_eligible;
  std::set<std::string, std::less<>> zig_eligible;
  for (auto const& [key, leaf] : *cpp) {
    if (detail::eligible(leaf)) {
      cpp_eligible.insert(key);
    }
  }
  for (auto const& [key, leaf] : *zig) {
    if (detail::eligible(leaf)) {
      zig_eligible.insert(key);
    }
  }
  if (cpp_eligible != zig_eligible) {
    error = std::format("eligible catalog inventory differs: cpp={} zig={}", cpp_eligible, zig_eligible);
    return std::nullopt;
  }

  std::vector<step> result;
  result.reserve(cpp_eligible.size());
  for (auto const& key : cpp_eligible) {
    auto const& leaf = cpp->at(key);
    step        generated{.key = key, .args = leaf.path};
    if (leaf.has_json) {
      generated.args.emplace_back("--json");
    }
    result.push_back(std::move(generated));
  }
  return result;
}

/// @brief Verify a reported generated inventory against both catalogs.
/// @param steps The argv steps actually scheduled for the state lane.
/// @param cpp_json The C++ binary's catalog.
/// @param zig_json The Zig oracle's catalog.
/// @param error Receives invented or missing paths.
/// @return `true` only for an exact bijection.
inline auto verify_inventory(std::span<const step> steps, std::string_view cpp_json, std::string_view zig_json,
                             std::string& error) -> bool {
  auto expected = generated_steps(cpp_json, zig_json, error);
  if (!expected) {
    return false;
  }
  std::set<std::string, std::less<>> got;
  for (auto const& item : steps) {
    if (!got.insert(item.key).second) {
      error = std::format("generated inventory repeats leaf {}", item.key);
      return false;
    }
  }
  std::set<std::string, std::less<>> wanted;
  for (auto const& item : *expected) {
    wanted.insert(item.key);
  }
  if (got != wanted) {
    std::set<std::string, std::less<>> invented;
    std::set<std::string, std::less<>> missing;
    std::set_difference(got.begin(), got.end(), wanted.begin(), wanted.end(), std::inserter(invented, invented.end()));
    std::set_difference(wanted.begin(), wanted.end(), got.begin(), got.end(), std::inserter(missing, missing.end()));
    error = std::format("generated inventory is not a catalog bijection: invented={} missing={}", invented, missing);
    return false;
  }
  return true;
}

} // namespace planar::cmd::state_catalog
