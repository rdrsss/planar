/// @file closeout.cppm
/// @brief CLI declaration for `planar plan closeout`.
export module planar.cmd.planar.handlers.plan.closeout;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the closeout CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_closeout(CLI::App& plan) -> CLI::App* {
  CLI::App* closeout = plan.add_subcommand(
      "closeout",
      "Evaluate the DB-hard gate (all tasks terminal, all descendants terminal, no live claims)\n  and advisory git-evidence for "
      "a plan. In apply mode (no --dry-run), marks the plan\n  done when the hard gate passes. Cancelled tasks are terminal "
      "\xe2\x80\x94 they do not block.\n\n  Hard gate failures exit non-zero in APPLY mode. --dry-run always exits 0:\n  it is a "
      "preview, and the caller reads ready/blocked_by from the report.\n\n  "
      "--check-merge adds an advisory epic-branch merge roll-up: for each contributing\n  branch from agent_work_claims, reports "
      "how many are merged to the target branch.\n  Never blocks; absent branches are inconclusive.");
  add_bool(*closeout, "--dry-run", "Evaluate and report only; never writes.");
  add_bool(*closeout, "--check-merge", "Include advisory epic-branch merge roll-up in the output.");
  add_json(*closeout, k_undocumented);
  add_positional(*closeout, "plan-id", k_undocumented);
  return closeout;
}
} // namespace planar::cmd::handlers::plan_cli
