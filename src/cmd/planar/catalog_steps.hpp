// @file catalog_steps.hpp
// @brief Catalog-to-argv conversion, and the inventory gate over it.
//
// The rule this enforces is that nothing may manufacture a command name.
// Invocations are DERIVED from a `planar schema` document and are admitted
// only when the catalog says no argument is required; `verify_inventory`
// then refuses any argv that is not itself a catalog leaf, and
// `verify_partition` refuses an inventory that double-counts or omits one.
//
// ## Provenance (plan 996, task 6045; decisions 963/982)
//
// This header was written for `statediff.t.cpp`, the C++/Zig DATABASE-STATE
// differential lane, and its two-catalog signature comes from that lane
// comparing the C++ tree's `schema` output against the oracle's. That lane
// and its oracle were deleted at the M10 cutover. The header SURVIVED it:
// none of these functions ever ran a binary, read `oracle_available()`, or
// knew what produced the bytes it parses. The second catalog is simply "the
// document the first must agree with" — its live callers, in
// `surface_generator.t.cpp`, pass hand-built fixtures.
//
// The alternative was folding it into that one test file. It is 400 lines
// of parsing and set algebra with its own failure vocabulary, exercised by
// six cases that each reach a different arm; inlining it would have buried
// the subject inside its own tests.
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
  // A PORT-SCOPE CARVEOUT DOES NOT BELONG HERE, and the reason outlives the
  // oracle that made it concrete. Plan 996, task 6419 wanted an entry for
  // the `ext`/`sync` leaves that had moved to `planar-ext`. Adding one
  // would have BROKEN the check rather than relaxed it: `excluded_steps`
  // requires `cpp_excluded == other_excluded` exactly, and those leaves
  // were one-sided (present in only one of the two catalogs), so the
  // partition check would have failed on the asymmetry. One-sided leaves
  // are stripped from the input document by the caller, never excused
  // here.
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
/// @param other_json A second `schema` catalog document that must declare
/// the same eligible inventory. Historically the Zig oracle's; after the M10
/// cutover (decisions 963/982) callers pass a fixture, or the same document
/// twice, and the parameter survives as the disagreement arm's input.
/// @param error Receives a refusal suitable for a test diagnostic.
/// @return Generated argv steps, or unset before either subject binary runs.
inline auto generated_steps(std::string_view cpp_json, std::string_view other_json, std::string& error)
    -> std::optional<std::vector<step>> {
  error.clear();
  auto cpp = detail::leaves(cpp_json, error);
  if (!cpp) {
    return std::nullopt;
  }
  auto other = detail::leaves(other_json, error);
  if (!other) {
    return std::nullopt;
  }

  std::set<std::string, std::less<>> cpp_eligible;
  std::set<std::string, std::less<>> other_eligible;
  for (auto const& [key, leaf] : *cpp) {
    if (detail::eligible(leaf)) {
      cpp_eligible.insert(key);
    }
  }
  for (auto const& [key, leaf] : *other) {
    if (detail::eligible(leaf)) {
      other_eligible.insert(key);
    }
  }
  if (cpp_eligible != other_eligible) {
    error = std::format("eligible catalog inventory differs: cpp={} other={}", cpp_eligible, other_eligible);
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
/// @return The matching exclusions, or unset if the two catalogs disagree.
inline auto excluded_steps(std::string_view cpp_json, std::string_view other_json, std::string& error)
    -> std::optional<std::vector<exclusion>> {
  error.clear();
  auto cpp = detail::leaves(cpp_json, error);
  if (!cpp) {
    return std::nullopt;
  }
  auto other = detail::leaves(other_json, error);
  if (!other) {
    return std::nullopt;
  }

  std::map<std::string, exclusion, std::less<>> cpp_excluded;
  std::map<std::string, exclusion, std::less<>> other_excluded;
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
  if (!collect(*cpp, cpp_excluded) || !collect(*other, other_excluded)) {
    return std::nullopt;
  }
  const auto same_exclusions = [&] {
    if (cpp_excluded.size() != other_excluded.size()) {
      return false;
    }
    for (auto const& [key, item] : cpp_excluded) {
      auto const found = other_excluded.find(key);
      if (found == other_excluded.end() || item.path != found->second.path || item.reason != found->second.reason) {
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
/// @param other_json The second catalog to agree with (see `generated_steps`).
/// @param error Receives invented or missing paths.
/// @return `true` only for an exact bijection.
inline auto verify_inventory(std::span<const step> steps, std::string_view cpp_json, std::string_view other_json,
                             std::string& error) -> bool {
  auto expected = generated_steps(cpp_json, other_json, error);
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
                             std::string_view cpp_json, std::string_view other_json, std::string& error) -> bool {
  auto expected = generated_steps(cpp_json, other_json, error);
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
