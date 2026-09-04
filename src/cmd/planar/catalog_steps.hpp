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

/// @brief One catalog leaf and the exact argv generated for it.
struct step {
  std::vector<std::string> path; ///< Catalog path tokens, excluding argv[0].
  std::vector<std::string> args; ///< Exact generated argv elements, excluding argv[0].
};

/// @brief A catalog leaf deliberately outside automated state execution.
struct exclusion {
  std::vector<std::string> path;
  std::string_view         reason;
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

inline auto argv_key(std::span<const std::string> args, std::string& error) -> std::optional<std::string> {
  auto encoded = glz::write_json(args);
  if (!encoded) {
    error = "serializing generated argv failed";
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

inline auto exclusion_reason(std::span<const std::string> path) -> std::optional<std::string_view> {
  // These are execution-environment exceptions, not port-scope carveouts.
  // Decision 980 is the sole port-scope exception (`explore`).  Each entry
  // remains checked and reported by `excluded_steps`; adding an omission here
  // therefore cannot make it disappear from the catalog evidence.
  //
  // Plan 996, task 6419 deliberately does NOT add an entry here for the
  // `ext`/`sync` leaves that moved to `planar-ext`: unlike `explore` (which
  // exists, unexecuted, on BOTH sides), those leaves now exist ONLY in the
  // oracle's catalog — `excluded_steps` requires `cpp_excluded ==
  // zig_excluded` exactly, so a one-sided entry here would fail that
  // partition check rather than pass it. The state-differential tests
  // strip those catalog entries out of the oracle's JSON before ever
  // reaching this function; see `strip_moved_ext_sync_leaves` in
  // `statediff.t.cpp`.
  const auto is = [&](std::initializer_list<std::string_view> wanted) {
    return path.size() == wanted.size() && std::ranges::equal(path, wanted);
  };
  if (is({"explore"})) {
    return "deferred-by-decision980";
  }
  if (is({"config", "edit"})) {
    return "interactive-editor";
  }
  if (is({"schema"}) || is({"skills"}) || is({"version"})) {
    return "output-only";
  }
  if (is({"assoc", "detect"}) || is({"scope", "suggest"})) {
    return "arena-path-output-only";
  }
  return std::nullopt;
}

inline auto eligible(const leaf& candidate) -> bool {
  return !candidate.has_required_input && !exclusion_reason(candidate.path).has_value();
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
    step        generated{.path = leaf.path, .args = leaf.path};
    if (leaf.has_json) {
      generated.args.emplace_back("--json");
    }
    result.push_back(std::move(generated));
  }
  return result;
}

/// @brief Report every catalog-valid no-input leaf intentionally excluded from state execution.
/// @return Matching C++/Zig exclusions, or unset if the catalogs disagree.
inline auto excluded_steps(std::string_view cpp_json, std::string_view zig_json, std::string& error)
    -> std::optional<std::vector<exclusion>> {
  error.clear();
  auto cpp = detail::leaves(cpp_json, error);
  if (!cpp) {
    return std::nullopt;
  }
  auto zig = detail::leaves(zig_json, error);
  if (!zig) {
    return std::nullopt;
  }

  std::map<std::string, exclusion, std::less<>> cpp_excluded;
  std::map<std::string, exclusion, std::less<>> zig_excluded;
  const auto                                    collect = [&](const auto& leaves, auto& out) -> bool {
    for (auto const& [key, candidate] : leaves) {
      if (candidate.has_required_input) {
        continue;
      }
      auto reason = detail::exclusion_reason(candidate.path);
      if (reason && !out.emplace(key, exclusion{.path = candidate.path, .reason = *reason}).second) {
        error = "catalog declares a duplicate exclusion";
        return false;
      }
    }
    return true;
  };
  if (!collect(*cpp, cpp_excluded) || !collect(*zig, zig_excluded)) {
    return std::nullopt;
  }
  const auto same_exclusions = [&] {
    if (cpp_excluded.size() != zig_excluded.size()) {
      return false;
    }
    for (auto const& [key, item] : cpp_excluded) {
      auto const found = zig_excluded.find(key);
      if (found == zig_excluded.end() || item.path != found->second.path || item.reason != found->second.reason) {
        return false;
      }
    }
    return true;
  };
  if (!same_exclusions()) {
    error = "catalog exclusion partition differs";
    return std::nullopt;
  }

  std::vector<exclusion> result;
  result.reserve(cpp_excluded.size());
  for (auto const& [_, item] : cpp_excluded) {
    result.push_back(item);
  }
  return result;
}

/// @brief Verify that every reported generated argv exactly matches its catalog leaf.
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
  std::map<std::string, std::string, std::less<>> expected_by_argv;
  std::set<std::string, std::less<>>              wanted;
  for (auto const& item : *expected) {
    auto path = detail::path_key(item.path, error);
    auto argv = detail::argv_key(item.args, error);
    if (!path || !argv || !expected_by_argv.emplace(*argv, *path).second) {
      if (error.empty()) {
        error = "catalog generated duplicate argv";
      }
      return false;
    }
    wanted.insert(*path);
  }

  std::set<std::string, std::less<>> got;
  for (auto const& item : steps) {
    auto argv = detail::argv_key(item.args, error);
    if (!argv) {
      return false;
    }
    auto catalog = expected_by_argv.find(*argv);
    if (catalog == expected_by_argv.end()) {
      error = std::format("generated argv is not a catalog leaf: {}", item.args);
      return false;
    }
    if (!got.insert(catalog->second).second) {
      error = std::format("generated inventory repeats executed argv {}", item.args);
      return false;
    }
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

/// @brief Resolve a manually executed argv vector to one eligible catalog leaf.
/// @return The longest matching catalog path, or unset for a non-eligible/refusal argv.
inline auto stateful_leaf(std::span<const std::string> args, std::span<const step> eligible, std::string& error)
    -> std::optional<step> {
  std::optional<step> match;
  for (auto const& candidate : eligible) {
    if (args.size() < candidate.path.size() || !std::ranges::equal(candidate.path, args.first(candidate.path.size()))) {
      continue;
    }
    if (match && match->path.size() == candidate.path.size()) {
      error = std::format("manual argv resolves ambiguously: {}", args);
      return std::nullopt;
    }
    if (!match || candidate.path.size() > match->path.size()) {
      match = candidate;
    }
  }
  return match;
}

/// @brief Verify the disjoint generated/stateful/malformed coverage partition.
///
/// Generated entries must retain the exact argv derived from the catalog.
/// Stateful entries are canonical catalog leaves covered by an explicitly
/// ordered manual execution, while malformed entries must resolve to no
/// eligible leaf at all.  The first two partitions must cover every eligible
/// catalog leaf exactly once.
inline auto verify_partition(std::span<const step> generated, std::span<const step> stateful, std::span<const step> malformed,
                             std::string_view cpp_json, std::string_view zig_json, std::string& error) -> bool {
  auto expected = generated_steps(cpp_json, zig_json, error);
  if (!expected) {
    return false;
  }

  std::map<std::string, step, std::less<>>        expected_by_path;
  std::map<std::string, std::string, std::less<>> expected_path_by_argv;
  for (auto const& item : *expected) {
    auto path = detail::path_key(item.path, error);
    auto argv = detail::argv_key(item.args, error);
    if (!path || !argv || !expected_by_path.emplace(*path, item).second || !expected_path_by_argv.emplace(*argv, *path).second) {
      if (error.empty()) {
        error = "catalog generated duplicate identity";
      }
      return false;
    }
  }

  std::set<std::string, std::less<>> generated_keys;
  for (auto const& item : generated) {
    auto argv = detail::argv_key(item.args, error);
    if (!argv) {
      return false;
    }
    auto const found = expected_path_by_argv.find(*argv);
    if (found == expected_path_by_argv.end()) {
      error = std::format("generated argv is not a catalog leaf: {}", item.args);
      return false;
    }
    if (!generated_keys.insert(found->second).second) {
      error = std::format("generated inventory repeats executed argv {}", item.args);
      return false;
    }
  }

  std::set<std::string, std::less<>> stateful_keys;
  for (auto const& item : stateful) {
    auto path = detail::path_key(item.path, error);
    if (!path) {
      return false;
    }
    auto const found = expected_by_path.find(*path);
    if (found == expected_by_path.end() || item.args != found->second.args) {
      error = std::format("stateful inventory is not a canonical catalog leaf: {}", item.path);
      return false;
    }
    if (!stateful_keys.insert(*path).second) {
      error = std::format("stateful inventory repeats leaf {}", item.path);
      return false;
    }
  }

  for (auto const& item : malformed) {
    std::string resolve_error;
    auto        resolved = stateful_leaf(item.args, *expected, resolve_error);
    if (!resolve_error.empty()) {
      error = resolve_error;
      return false;
    }
    if (resolved) {
      error = std::format("malformed argv overlaps eligible catalog leaf: {}", item.args);
      return false;
    }
  }

  for (auto const& key : generated_keys) {
    if (stateful_keys.contains(key)) {
      error = std::format("generated and stateful inventories overlap at {}", key);
      return false;
    }
  }
  std::set<std::string, std::less<>> covered = generated_keys;
  covered.insert(stateful_keys.begin(), stateful_keys.end());
  std::set<std::string, std::less<>> wanted;
  for (auto const& [key, _] : expected_by_path) {
    wanted.insert(key);
  }
  if (covered != wanted) {
    error = std::format("catalog coverage partition differs: covered={} wanted={}", covered, wanted);
    return false;
  }
  return true;
}

} // namespace planar::cmd::state_catalog
