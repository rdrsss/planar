/// @file queuerule.cpp
/// @brief Implementation of `planar.queuerule` (see queuerule.cppm).
/// `queue-rule.md` is embedded via `#embed`, the same mechanism as
/// `planar.engine.config.effective`'s `defaults.toml`: one fixed file beside
/// the unit.

module;

module planar.queuerule;

import std;

namespace planar::queuerule {
namespace {

constexpr unsigned char k_rule_bytes[] = {
#embed "queue-rule.md"
};

} // namespace

auto queue_rule_text() -> std::string_view {
  // Built per call, not held in a namespace-scope view: `engine_workspace`
  // reads this from its own static initialiser, and a view initialised
  // dynamically in this unit could still be empty at that point. The byte
  // array is constant-initialised, so this is safe at any time. Not
  // `constexpr` because `reinterpret_cast` is not a core constant expression.
  return {reinterpret_cast<const char*>(k_rule_bytes), sizeof(k_rule_bytes)};
}

} // namespace planar::queuerule
