/// @file propagate.cpp
/// @brief Implementation of `planar.engine.extsync.propagate`.

module planar.engine.extsync.propagate;

import std;

namespace planar::engine::extsync::propagate {

auto strategy_for_system(std::string_view system_kind) -> std::optional<strategy> {
  if (system_kind == "jira") {
    return strategy{.kind = "jira-epic", .plan_anchor_kind = "epic", .plan_child_kind = "story", .task_kind = "sub-task"};
  }
  if (system_kind == "github-issues") {
    // ALWAYS `github-parent-issue`, never the zero-repo or projects-v2
    // bucket — those are `selectStrategy`'s job and it is not ported. See
    // this module's header.
    return strategy{
        .kind = "github-parent-issue", .plan_anchor_kind = "parent-issue", .plan_child_kind = "issue", .task_kind = "sub-task"};
  }
  return std::nullopt;
}

} // namespace planar::engine::extsync::propagate
