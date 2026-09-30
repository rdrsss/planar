/// @file rule.cpp
/// @brief Implementation of `planar.engine.hostqueue.rule` (see rule.cppm).
/// `queue-rule.md` is embedded via `#embed`, the same mechanism as
/// `planar.engine.config.effective`'s `defaults.toml`: one fixed file beside
/// the unit, so no generated unit is needed.

module;

module planar.engine.hostqueue.rule;

import std;

namespace planar::engine::hostqueue {
namespace {

constexpr unsigned char k_rule_bytes[] = {
#embed "queue-rule.md"
};

// Not `constexpr`: built through `reinterpret_cast` over the `#embed`'d bytes,
// which is not a core constant expression. The array is statically allocated
// and the view is initialised once, before main().
const std::string_view k_rule_text(reinterpret_cast<const char*>(k_rule_bytes), sizeof(k_rule_bytes));

} // namespace

auto queue_rule_text() -> std::string_view {
  return k_rule_text;
}

} // namespace planar::engine::hostqueue
