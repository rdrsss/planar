// @file catalog_parity.hpp
// @brief Compare a C++ binary's command-tree DECLARATIONS against the Zig
// oracle's, through the `schema` JSON catalog both emit (plan 996, task
// 6123).
//
// ## Why this exists, and what it replaces
//
// Before task 6123 each binary's `parity.t.cpp` proved its tree was
// transcribed correctly by diffing every leaf's rendered `--help` page
// against the oracle's, byte for byte. That worked because `src/lib/cli`
// was a port of etcli's help renderer, so identical declarations produced
// identical bytes. Task 6123 deleted that renderer — CLI11 renders help
// now — so those diffs can no longer pass, and the operator sanctioned
// re-baselining the pages onto CLI11's output.
//
// Re-baselining the PAGES would have quietly dropped what those cases were
// actually for. The page was never the point; it was the OBSERVABLE
// PROXY for "does this tree declare the same flags, required-ness,
// positionals and subcommands the oracle declares?" — a transcription slip
// in `tree.cpp` showed up as changed bytes. That question is still fully
// answerable, and answerable MORE DIRECTLY, because both trees emit a
// `schema` catalog and `planar.cliapp.schema` deliberately reproduces the
// oracle's key set (see its header for the measured verdict).
//
// So this header compares the DECLARATIONS, not the rendering:
//
//   * every command the C++ tree exposes must exist in the oracle's catalog
//   * with the same flag set (by canonical long name)
//   * the same REQUIRED flags
//   * the same positionals, in order, with the same required-ness
//   * and the same subcommand list, restricted to the ported subset
//
// The last restriction is the one honest concession: every C++ tree here is
// a SUBSET of the oracle's (fourteen of eighteen agent verbs, three of
// twelve watch verbs, seven of forty-seven planar verbs), so a parent's
// subcommand list legitimately differs. The comparison therefore requires
// the C++ subcommand list to be a SUBSET of the oracle's — which still
// catches an invented verb, a misspelled one, or one attached under the
// wrong parent, and is the strongest statement that is actually true.
//
// ## Why a HEADER
//
// Same reason as `parity_harness.hpp` beside it, and its header states the
// argument in full: D18 forbids a `cmd_* -> cmd_*` edge, a shared layer-1
// library would put test-only scaffolding in the shipped base layer, and a
// plain header creates no target edge at all. Nothing that ships includes
// this.
#pragma once

// This header is included from module-importing test translation units, so
// it must not `#include` any standard library header — `import std;` at the
// top of the includer provides everything used below. Glaze is not a
// module, so its include stays here.
#include <glaze/glaze.hpp>

namespace planar::cmd::parity {

/// @brief One command's declared surface, extracted from a catalog entry.
struct command_surface {
  std::vector<std::string> flags;          ///< Canonical long names, sorted.
  std::vector<std::string> required_flags; ///< The subset marked required, sorted.
  /// @brief `(name, required)` in DECLARATION order — order is load-bearing
  /// for positionals in a way it is not for flags.
  std::vector<std::pair<std::string, bool>> positionals;
  std::vector<std::string>                  subcommands; ///< Child names, sorted.
};

/// @brief Every command in a `<bin> schema` catalog, keyed by its
/// `"command"` path string (e.g. `"planar workbench push"`).
using catalog_map = std::map<std::string, command_surface, std::less<>>;

/// @brief Parse a catalog document into `(command path -> surface)`.
///
/// Deliberately tolerant of the keys it does NOT read: the catalog carries
/// `kind`, `choices`, `default`, `description`, `docs` and more, none of
/// which this comparison asserts on. Those are the fields where the CLI11
/// swap legitimately moved things (see `planar.cliapp.schema`'s header for
/// the four named divergences), and asserting on them would re-introduce
/// the coupling this header exists to remove.
/// @param text The catalog JSON.
/// @return The parsed surfaces, or unset when the document does not parse.
inline auto parse_catalog(std::string const& text) -> std::optional<catalog_map> {
  auto parsed = glz::read_json<glz::generic>(text);
  if (!parsed) {
    return std::nullopt;
  }
  glz::generic const& doc = *parsed;
  if (!doc.contains("commands")) {
    return std::nullopt;
  }
  catalog_map out;
  for (auto const& entry : doc.at("commands").get<glz::generic::array_t>()) {
    if (!entry.contains("command")) {
      continue;
    }
    command_surface surface;
    if (entry.contains("flags")) {
      for (auto const& flag : entry.at("flags").get<glz::generic::array_t>()) {
        if (!flag.contains("long")) {
          continue;
        }
        auto name = flag.at("long").get<std::string>();
        surface.flags.push_back(name);
        if (flag.contains("required") && flag.at("required").is_boolean() && flag.at("required").get<bool>()) {
          surface.required_flags.push_back(std::move(name));
        }
      }
    }
    if (entry.contains("positionals")) {
      for (auto const& positional : entry.at("positionals").get<glz::generic::array_t>()) {
        if (!positional.contains("name")) {
          continue;
        }
        bool const required =
            positional.contains("required") && positional.at("required").is_boolean() && positional.at("required").get<bool>();
        surface.positionals.emplace_back(positional.at("name").get<std::string>(), required);
      }
    }
    if (entry.contains("subcommands")) {
      for (auto const& child : entry.at("subcommands").get<glz::generic::array_t>()) {
        surface.subcommands.push_back(child.get<std::string>());
      }
    }
    std::ranges::sort(surface.flags);
    std::ranges::sort(surface.required_flags);
    std::ranges::sort(surface.subcommands);
    out.emplace(entry.at("command").get<std::string>(), std::move(surface));
  }
  return out;
}

/// @brief Compare every command the C++ catalog declares against the
/// oracle's declaration of the same command.
/// @param cpp_catalog The C++ binary's catalog.
/// @param oracle_catalog The Zig reference binary's catalog.
/// @return One human-readable line per disagreement; empty when the C++
/// tree's declarations are a faithful subset of the oracle's.
inline auto diff_against_oracle(catalog_map const& cpp_catalog, catalog_map const& oracle_catalog) -> std::vector<std::string> {
  std::vector<std::string> problems;
  for (auto const& [command, mine] : cpp_catalog) {
    auto const found = oracle_catalog.find(command);
    if (found == oracle_catalog.end()) {
      problems.push_back(std::format("`{}` exists in the C++ tree but NOT in the oracle's catalog", command));
      continue;
    }
    auto const& theirs = found->second;

    if (mine.flags != theirs.flags) {
      problems.push_back(std::format("`{}` flags: cpp {} vs oracle {}", command, mine.flags, theirs.flags));
    }
    if (mine.required_flags != theirs.required_flags) {
      problems.push_back(
          std::format("`{}` required flags: cpp {} vs oracle {}", command, mine.required_flags, theirs.required_flags));
    }
    if (mine.positionals != theirs.positionals) {
      std::vector<std::string> mine_text;
      std::vector<std::string> theirs_text;
      for (auto const& [name, required] : mine.positionals) {
        mine_text.push_back(std::format("{}{}", name, required ? " (required)" : ""));
      }
      for (auto const& [name, required] : theirs.positionals) {
        theirs_text.push_back(std::format("{}{}", name, required ? " (required)" : ""));
      }
      problems.push_back(std::format("`{}` positionals: cpp {} vs oracle {}", command, mine_text, theirs_text));
    }
    for (auto const& child : mine.subcommands) {
      if (!std::ranges::contains(theirs.subcommands, child)) {
        problems.push_back(std::format("`{}` declares subcommand `{}`, which the oracle does not", command, child));
      }
    }
  }
  return problems;
}

} // namespace planar::cmd::parity
