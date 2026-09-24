/// @file coverage.cpp
/// @brief Implementation of `planar.engine.ingest.coverage` (see coverage.cppm).

module planar.engine.ingest.coverage;

import std;
import planar.engine.ingest.diff;

namespace planar::engine::ingest::coverage {

auto coverage::has_gaps() const -> bool {
  return !uncovered_task_slugs_.empty() || !orphan_scenarios_.empty();
}

auto compute(const diff::diff& d) -> coverage {
  // Slugs cited by at least one scenario. Numeric refs contribute nothing —
  // they cannot be tied back to a roadmap bullet at preview time.
  std::set<std::string, std::less<>> cited;
  for (const auto& scenario : d.scenarios_) {
    for (const auto& ref : scenario.verifies_) {
      if (!ref.slug_.empty()) {
        cited.insert(ref.slug_);
      }
    }
  }

  coverage result{};
  // `std::set` supplies both the dedupe and the bytewise ordering the JSON
  // projection is pinned to; sorting a vector afterwards would be equivalent
  // but leaves the dedupe implicit.
  std::set<std::string, std::less<>> uncovered;

  for (const auto& plan : d.child_plans_) {
    for (const auto& task : plan.tasks_) {
      if (task.op_ != diff::op::add && task.op_ != diff::op::update) {
        continue;
      }
      ++result.total_tasks_;
      if (task.slug_.empty()) {
        ++result.tasks_without_slug_;
        continue;
      }
      ++result.tasks_with_slug_;
      if (!cited.contains(task.slug_)) {
        uncovered.insert(task.slug_);
      }
    }
  }
  result.uncovered_task_slugs_.assign(uncovered.begin(), uncovered.end());

  // Orphan scenarios: those citing nothing at all. Deliberately NOT deduped —
  // two distinct scenarios may share a title, and collapsing them would
  // under-report the gap the gate exists to surface.
  for (const auto& scenario : d.scenarios_) {
    if (scenario.verifies_.empty()) {
      result.orphan_scenarios_.push_back(scenario.title_);
    }
  }
  std::ranges::sort(result.orphan_scenarios_);

  return result;
}

} // namespace planar::engine::ingest::coverage
