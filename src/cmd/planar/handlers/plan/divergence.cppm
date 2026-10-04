/// @file divergence.cppm
/// @brief CLI declaration for `planar plan divergence`.
export module planar.cmd.planar.handlers.plan.divergence;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the divergence CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_divergence(CLI::App& plan) -> CLI::App* {
  CLI::App* divergence = plan.add_subcommand(
      "divergence",
      "Report the declared-vs-derived closure divergence for a plan's open tasks.\n\n  For every unordered pair of open (todo) "
      "tasks, compares whether the two\n  tasks overlap under the DECLARED touch set vs. the DERIVED closure set.\n  A pair "
      "whose verdict differs between sources is a FLIP \xe2\x80\x94 the two sources\n  disagree about whether those tasks can "
      "run in parallel.\n\n  Jaccard distance = flips / |declared_overlaps \xe2\x88\xaa derived_overlaps|.\n  0.0 = sources "
      "agree on every pair; 1.0 = no overlapping pair in common.\n\n  READ-ONLY: computes and reports; writes nothing.");
  add_json(*divergence, "Emit machine-readable JSON instead of text");
  add_positional(*divergence, "plan-id", "Plan id or slug");
  return divergence;
}
} // namespace planar::cmd::handlers::plan_cli
