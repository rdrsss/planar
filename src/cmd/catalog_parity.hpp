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
//   * with the same flags, IN DECLARATION ORDER, agreeing on canonical long
//     name, required-ness, kind, short form, aliases, choice set,
//     repeatability, value-name placeholder, count/hidden/deprecated/env
//     and completion metadata
//   * the same positionals, in order, with the same required-ness, kind and
//     description
//   * the same subcommand list, IN DECLARATION ORDER
//   * and the same `summary` and `description` strings
//
// ## What task 6065 changed here, and why the concession is gone
//
// This header used to end with an honest concession: every C++ tree was a
// SUBSET of the oracle's (fourteen of eighteen agent verbs, three of twelve
// watch verbs, seven of forty-seven planar verbs), so a parent's subcommand
// list legitimately differed, and the comparison could only require the C++
// list to be a SUBSET. It also compared flags as SORTED SETS of long names
// plus a required-set, and ignored description, kind, order and everything
// else.
//
// Both limits are lifted, because the surface is no longer a subset:
// `planar` declares all 223 leaves, `planar-agent` all 24, `planar-watch`
// all 13. So the comparison is now
//
//   * TWO-DIRECTIONAL — `oracle_only_commands` reports anything the oracle
//     declares and the C++ tree does not, which is the check that has teeth
//     only once full-surface parity is claimed; and
//   * ORDER-SENSITIVE — CLI11 renders help and this repo's catalog both in
//     insertion order, so a subcommand or flag list in a different order is
//     a visible difference, not a formatting detail.
//
// ## What it still does NOT compare, measured rather than assumed
//
// `default`, and only `default`. Four oracle flags declare an EMPTY-STRING
// default (`planar workbench edit --editor`, and `--args` / `--worktree` /
// `--sandbox-root` on `planar workflow run`) and report `""` where this
// tree reports `null`. `CLI::Option` exposes ONE accessor,
// `get_default_str()`, returning `""` for both "no default" and "a default
// that is the empty string" — the two are not distinguishable from a built
// tree, and mapping empty to `""` would make all ~500 flags with no default
// report one. Every OTHER flag field, including the bool/int/string typing
// of the default literal, does agree; see `planar.cliapp.schema`'s
// `default_literal`. With `default` excluded, `planar-agent` and
// `planar-watch` emit catalogs BYTE-IDENTICAL to the oracle's, and
// `planar`'s differs in exactly those four values.
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

/// @brief One flag's declared surface, rendered to a single comparable
/// string so a mismatch reports WHICH field moved rather than "these two
/// lists differ".
///
/// `default` is deliberately absent — see this header's account of the four
/// empty-string defaults CLI11 cannot express.
struct flag_surface {
  std::string name; ///< Canonical long name, for locating a mismatch.
  std::string rendered;

  auto operator==(flag_surface const&) const -> bool = default;
};

/// @brief One positional's declared surface, same shape and same reason.
struct positional_surface {
  std::string name;
  std::string rendered;

  auto operator==(positional_surface const&) const -> bool = default;
};

/// @brief One command's declared surface, extracted from a catalog entry.
struct command_surface {
  std::string                     summary;     ///< The one-line summary.
  std::string                     description; ///< The long description.
  std::vector<flag_surface>       flags;       ///< In DECLARATION order.
  std::vector<positional_surface> positionals; ///< In DECLARATION order.
  std::vector<std::string>        subcommands; ///< Child names, in DECLARATION order.
  std::vector<std::string>        aliases;     ///< Command aliases, in declaration order.
};

/// @brief Every command in a `<bin> schema` catalog, keyed by its
/// `"command"` path string (e.g. `"planar workbench push"`).
using catalog_map = std::map<std::string, command_surface, std::less<>>;

/// @brief Render every key of a catalog object EXCEPT those in `skip`,
/// in a stable `key=json` form.
///
/// Written as an exclusion rather than an inclusion on purpose: a new key
/// added to the emitter but not to the oracle (or vice versa) then shows up
/// as a mismatch instead of being silently ignored, which an
/// allow-list-shaped reader could never catch.
/// @param obj The flag or positional object.
/// @param skip Keys to leave out.
/// @return The rendered text.
inline auto render_fields(glz::generic const& obj, std::span<std::string_view const> skip) -> std::string {
  std::vector<std::string> parts;
  for (auto const& [key, value] : obj.get<glz::generic::object_t>()) {
    if (std::ranges::contains(skip, std::string_view{key})) {
      continue;
    }
    std::string encoded;
    if (!glz::write_json(value, encoded)) {
      parts.push_back(std::format("{}={}", key, encoded));
    }
  }
  std::ranges::sort(parts);
  std::string out;
  for (auto const& part : parts) {
    if (!out.empty()) {
      out += ' ';
    }
    out += part;
  }
  return out;
}

/// @brief Parse a catalog document into `(command path -> surface)`.
///
/// Reads every key the catalog carries on a flag or a positional except
/// `default` — see this header for the measured account of why that one is
/// excluded and what it costs. Command-level `hidden`, `deprecated`,
/// `flagGroups`, `docs`, `path` and `name` are left to the byte-comparison
/// the per-binary parity cases also run; this map exists to name WHICH
/// command and WHICH field moved.
/// @param text The catalog JSON.
/// @return The parsed surfaces, or unset when the document does not parse.
inline auto parse_catalog(std::string const& text) -> std::optional<catalog_map> {
  static constexpr std::string_view k_skip[] = {"default"};

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
    if (entry.contains("summary")) {
      surface.summary = entry.at("summary").get<std::string>();
    }
    if (entry.contains("description")) {
      surface.description = entry.at("description").get<std::string>();
    }
    if (entry.contains("flags")) {
      for (auto const& flag : entry.at("flags").get<glz::generic::array_t>()) {
        if (!flag.contains("long")) {
          continue;
        }
        surface.flags.emplace_back(flag.at("long").get<std::string>(), render_fields(flag, k_skip));
      }
    }
    if (entry.contains("positionals")) {
      for (auto const& positional : entry.at("positionals").get<glz::generic::array_t>()) {
        if (!positional.contains("name")) {
          continue;
        }
        surface.positionals.emplace_back(positional.at("name").get<std::string>(), render_fields(positional, k_skip));
      }
    }
    if (entry.contains("subcommands")) {
      for (auto const& child : entry.at("subcommands").get<glz::generic::array_t>()) {
        surface.subcommands.push_back(child.get<std::string>());
      }
    }
    if (entry.contains("aliases")) {
      for (auto const& alias : entry.at("aliases").get<glz::generic::array_t>()) {
        surface.aliases.push_back(alias.get<std::string>());
      }
    }
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

    if (mine.summary != theirs.summary) {
      problems.push_back(std::format("`{}` summary: cpp {:?} vs oracle {:?}", command, mine.summary, theirs.summary));
    }
    if (mine.description != theirs.description) {
      problems.push_back(std::format("`{}` description: cpp {:?} vs oracle {:?}", command, mine.description, theirs.description));
    }
    if (mine.aliases != theirs.aliases) {
      problems.push_back(std::format("`{}` aliases: cpp {} vs oracle {}", command, mine.aliases, theirs.aliases));
    }

    auto const mine_flag_names   = std::views::transform(mine.flags, &flag_surface::name) | std::ranges::to<std::vector>();
    auto const theirs_flag_names = std::views::transform(theirs.flags, &flag_surface::name) | std::ranges::to<std::vector>();
    if (mine_flag_names != theirs_flag_names) {
      problems.push_back(std::format("`{}` flags: cpp {} vs oracle {}", command, mine_flag_names, theirs_flag_names));
    } else {
      for (std::size_t i = 0; i < mine.flags.size(); ++i) {
        if (mine.flags[i].rendered != theirs.flags[i].rendered) {
          problems.push_back(std::format("`{}` flag `{}`: cpp [{}] vs oracle [{}]", command, mine.flags[i].name,
                                         mine.flags[i].rendered, theirs.flags[i].rendered));
        }
      }
    }

    auto const mine_pos_names =
        std::views::transform(mine.positionals, &positional_surface::name) | std::ranges::to<std::vector>();
    auto const theirs_pos_names =
        std::views::transform(theirs.positionals, &positional_surface::name) | std::ranges::to<std::vector>();
    if (mine_pos_names != theirs_pos_names) {
      problems.push_back(std::format("`{}` positionals: cpp {} vs oracle {}", command, mine_pos_names, theirs_pos_names));
    } else {
      for (std::size_t i = 0; i < mine.positionals.size(); ++i) {
        if (mine.positionals[i].rendered != theirs.positionals[i].rendered) {
          problems.push_back(std::format("`{}` positional `{}`: cpp [{}] vs oracle [{}]", command, mine.positionals[i].name,
                                         mine.positionals[i].rendered, theirs.positionals[i].rendered));
        }
      }
    }

    // EXACT SEQUENCE, not a subset and not a set. Order is what an
    // operator sees in `--help`, and find-or-create declaration (see
    // `planar.cliapp.surface`) can only preserve it because that module
    // reorders after the fact.
    if (mine.subcommands != theirs.subcommands) {
      problems.push_back(std::format("`{}` subcommands: cpp {} vs oracle {}", command, mine.subcommands, theirs.subcommands));
    }
  }
  return problems;
}

/// @brief Every command the ORACLE declares that the C++ catalog does not.
///
/// The other direction, and the one that only became meaningful at task
/// 6065: while each C++ tree was a deliberate SUBSET, this would have
/// listed ~190 entries by design and could assert nothing. Now that the
/// full surface is declared it must be empty, which is what makes
/// "`cli_usage_lint` can resolve every authored command path" an assertion
/// rather than a hope — the lint SKIPS a path it cannot resolve, so a
/// missing command silently removes that command's flags from the gate.
/// @param cpp_catalog The C++ binary's catalog.
/// @param oracle_catalog The Zig reference binary's catalog.
/// @return The missing command paths, sorted.
inline auto oracle_only_commands(catalog_map const& cpp_catalog, catalog_map const& oracle_catalog) -> std::vector<std::string> {
  std::vector<std::string> missing;
  for (auto const& [command, unused] : oracle_catalog) {
    if (!cpp_catalog.contains(command)) {
      missing.push_back(command);
    }
  }
  return missing;
}

} // namespace planar::cmd::parity
