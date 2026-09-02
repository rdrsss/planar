// @file oracle_retirement.hpp
// @brief Explicit, fail-closed evidence report for retiring the Zig oracle.
//
// This is deliberately test-only vocabulary.  The retirement decision is a
// reviewed cutover precondition, not a runtime switch that a production
// binary can accidentally take.  The C++ test lane supplies evidence from
// the catalog-derived state differential, the generated unported inventories,
// and the explicit oracle-skip inventory; this header says exactly when that
// evidence may authorize the *next* change to remove `zig/`.
#pragma once

namespace planar::cmd::oracle_retirement {

/// @brief Decision 982's in-scope port tail, excluding `explore`.
inline constexpr std::size_t in_scope_leaf_count = 11;

/// @brief The only permitted non-ported disposition at cutover.
inline constexpr std::string_view deferred_by_decision980 = "deferred-by-decision980";

/// @brief One explicitly reported unported command leaf.
struct unported_leaf {
  std::string_view binary;
  std::string_view path;
  std::string_view disposition;
};

/// @brief Inputs collected before the reviewed cutover change.
struct evidence {
  bool                              zig_tree_present = false;
  bool                              oracle_available = false;
  std::span<std::string_view const> accounted_state_leaves;
  std::span<std::string_view const> unexpected_state_deltas;
  std::span<unported_leaf const>    unported;
  std::span<std::string_view const> oracle_conditional_skips;
};

/// @brief A machine-readable-enough report for the cutover reviewer.
struct report {
  bool                     ready = false;
  std::vector<std::string> refusals;

  /// @brief Render every arm, including empty inventories, for review logs.
  /// @return Stable evidence text; `ready` never hides a refused arm.
  [[nodiscard]] auto render() const -> std::string {
    std::string out = std::format("oracle-retirement: {}\n", ready ? "READY" : "REFUSED");
    if (refusals.empty()) {
      out += "all evidence arms are clean\n";
      return out;
    }
    for (auto const& refusal : refusals) {
      out += std::format("refusal: {}\n", refusal);
    }
    return out;
  }
};

/// @brief Evaluate the complete oracle-retirement precondition.
///
/// The three decision arms are intentionally independent.  A clean state
/// lane cannot excuse an extra unported leaf, and neither can excuse a
/// remaining skip that would make a post-Zig parity test vacuous.  Requiring
/// the tree and oracle to exist here makes the evidence a pre-deletion fact:
/// no milestone number or after-the-fact test run can manufacture it.
/// @param input Explicit inventories collected by the evidence test.
/// @return `ready` only when every required arm is clean.
[[nodiscard]] inline auto evaluate(const evidence& input) -> report {
  report result;
  if (!input.zig_tree_present) {
    result.refusals.emplace_back("zig/ is absent; retirement evidence must be collected before deletion");
  }
  if (!input.oracle_available) {
    result.refusals.emplace_back("Zig oracle is unavailable; state evidence cannot be re-run");
  }

  std::set<std::string_view, std::less<>> accounted{input.accounted_state_leaves.begin(), input.accounted_state_leaves.end()};
  if (accounted.size() != in_scope_leaf_count || input.accounted_state_leaves.size() != in_scope_leaf_count) {
    result.refusals.emplace_back(
        std::format("state lane accounts for {}/{} distinct in-scope leaves", accounted.size(), in_scope_leaf_count));
  }
  if (!input.unexpected_state_deltas.empty()) {
    result.refusals.emplace_back(std::format("state lane has {} unexpected durable-state delta(s): {}",
                                             input.unexpected_state_deltas.size(), input.unexpected_state_deltas));
  }

  bool const only_deferred_explore = input.unported.size() == 1 && input.unported.front().binary == "planar" &&
                                     input.unported.front().path == "explore" &&
                                     input.unported.front().disposition == deferred_by_decision980;
  if (!only_deferred_explore) {
    std::string inventory;
    for (auto const& item : input.unported) {
      inventory += std::format("{}:{} ({}) ", item.binary, item.path, item.disposition);
    }
    result.refusals.emplace_back(std::format("unported inventory is not only planar:explore ({})", inventory));
  }

  if (!input.oracle_conditional_skips.empty()) {
    result.refusals.emplace_back(std::format("{} oracle-conditional parity skip(s) remain: {}",
                                             input.oracle_conditional_skips.size(), input.oracle_conditional_skips));
  }
  result.ready = result.refusals.empty();
  return result;
}

} // namespace planar::cmd::oracle_retirement
