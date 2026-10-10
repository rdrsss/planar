/// @file verb_classification.cpp
/// @brief Implementation of `planar.cmd.planar.verb_classification`.

module planar.cmd.planar.verb_classification;

import std;

namespace planar::cmd {

namespace {

/// @brief Is `sub` one of the read-shaped leaves shared by the
/// plan/task/question/... groups? Mirrors zig's `isReadLeaf`.
///
/// `done` is deliberately ABSENT even though it reads terminal: `task
/// done` is planning-class because coders must go through
/// `planar-agent complete`.
/// @param sub The subverb token.
/// @return True when the leaf is a read.
auto is_read_leaf(std::string_view sub) -> bool {
  static constexpr std::array<std::string_view, 12> k_reads{
      "show", "packet", "list", "view", "diff", "next", "recommend-strategy", "divergence", "tree", "review", "status", "log",
  };
  return std::ranges::find(k_reads, sub) != k_reads.end();
}

/// @brief Membership test over a fixed token list.
/// @param needle The token.
/// @param set The list.
/// @return True when present.
auto in(std::string_view needle, std::span<const std::string_view> set) -> bool {
  return std::ranges::find(set, needle) != set.end();
}

} // namespace

auto classify(std::span<const std::string> path) -> verb_class {
  if (path.empty()) {
    return verb_class::planning;
  }
  std::string_view const top = path[0];

  // Top-level verbs with no subverbs to inspect, or whose every subverb is
  // read-only. `explore` is the read-only launcher alias; `bench` is the
  // measurement rig, which the harness drives from inside worktrees by
  // design (the protected-instrument invariant) and would be defeated by a
  // refusal. `update` sits with `version`: it opens no database and writes
  // no planning state, only the install under `~/.planar/` through the
  // installer it execs, so a planning refusal would be false.
  static constexpr std::array<std::string_view, 13> k_top_reads{
      "resume", "dashboard",  "health", "report", "tree",       "search",  "version",
      "update", "completion", "schema", "import", "synthesize", "explore",
  };
  if (in(top, k_top_reads) || top == "bench") {
    return verb_class::execution_or_read;
  }

  // Subverb groups whose ENTIRE surface is execution_or_read. `workflow`
  // is a read-only filesystem scan holding no SQLite handle — discovering
  // and inspecting workflows is exactly the kind of read a coder's
  // worktree needs.
  static constexpr std::array<std::string_view, 14> k_read_groups{
      "handoff", "capture", "audit", "workspace", "config",    "models", "templates",
      "scope",   "doc",     "local", "skills",    "test-spec", "sync",   "workflow",
  };
  if (in(top, k_read_groups)) {
    return verb_class::execution_or_read;
  }

  // workbench: the methodology calls these five out explicitly. Anything
  // else under `workbench` (none today) defaults to planning.
  if (top == "workbench") {
    if (path.size() >= 2) {
      static constexpr std::array<std::string_view, 5> k_wb{"pull", "push", "status", "sync", "resolve"};
      if (in(path[1], k_wb)) {
        return verb_class::execution_or_read;
      }
    }
    return verb_class::planning;
  }

  if (top == "task" && path.size() >= 3 && path[1] == "touches" && path[2] == "list") {
    return verb_class::execution_or_read;
  }

  if (top == "feedback" && path.size() >= 3 && path[1] == "triage") {
    if (path[2] == "list" || path[2] == "show") {
      return verb_class::execution_or_read;
    }
    return verb_class::planning;
  }

  // Per-domain groups: reads allowed, everything else refused.
  static constexpr std::array<std::string_view, 8> k_entity_groups{
      "plan", "task", "question", "scenario", "decision", "artifact", "association", "assoc",
  };
  if (in(top, k_entity_groups)) {
    if (path.size() >= 2 && is_read_leaf(path[1])) {
      return verb_class::execution_or_read;
    }
    return verb_class::planning;
  }

  // ext: propagate / sync / push mutate EXTERNAL state, which is planning
  // intent. The read-side leaves are reads.
  if (top == "ext") {
    if (path.size() >= 2 && is_read_leaf(path[1])) {
      return verb_class::execution_or_read;
    }
    return verb_class::planning;
  }

  // annotate: every subverb mutates.
  if (top == "annotate") {
    return verb_class::planning;
  }

  static constexpr std::array<std::string_view, 6> k_planning_singletons{
      "init", "promote", "demote", "link", "unlink", "links",
  };
  if (in(top, k_planning_singletons)) {
    return verb_class::planning;
  }

  // Fall through: anything not yet seen is refused. A genuinely read-only
  // new top-level verb should be added above in the same change that
  // introduces it.
  return verb_class::planning;
}

} // namespace planar::cmd
