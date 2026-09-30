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

} // namespace planar::engine::hostqueue
