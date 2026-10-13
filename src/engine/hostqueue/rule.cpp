/// @file rule.cpp
/// @brief Implementation of `planar.engine.hostqueue.rule` (see rule.cppm).
/// The text is embedded by the layer-1 `planar.queuerule` module (task 7110);
/// this unit forwards to it.

module;

module planar.engine.hostqueue.rule;

import std;
import planar.queuerule;

namespace planar::engine::hostqueue {

auto queue_rule_text() -> std::string_view {
  return planar::queuerule::queue_rule_text();
}

auto queue_mandate_text() -> std::string_view {
  return planar::queuerule::queue_mandate_text();
}

auto render_queue_rule(const rule_policy& policy) -> std::string {
  std::string out{queue_rule_text()};
  if (policy.mandate) {
    out += '\n';
    out += queue_mandate_text();
    out += std::format("\nSource: operator policy use_queue = true ({})\n", policy.source);
  }
  if (!policy.notes.empty()) {
    out += '\n';
    for (auto const& note : policy.notes) {
      out += note;
      out += '\n';
    }
  }
  return out;
}

} // namespace planar::engine::hostqueue
