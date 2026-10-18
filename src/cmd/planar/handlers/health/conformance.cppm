/// @file conformance.cppm
/// @brief CLI declaration for `health conformance`.
export module planar.cmd.planar.handlers.health.conformance;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::health_cli {
/// @brief Register the conformance CLI node.
/// @param health Input health.
/// @return Registered CLI node.
export auto attach_conformance(CLI::App* health) -> CLI::App* {
  CLI::App* conformance = health->add_subcommand(
      "conformance",
      "Grade the recorded trail of one plan, its descendant plans, their tasks and their claims against the coordination\n  "
      "rules, whichever orchestrator produced it. Read-only.\n\n  Rules: 1 every done task had a claim that ended completed (a "
      "done made with --force is an override);\n  2 every "
      "claim ended in a terminal verb; 3 under require_model every coder claim records a model;\n  4 every worker claim carries "
      "a "
      "parent action; 5 under use_queue and task_gate every completed claim has a\n  queued gate run tagged with its token in "
      "the build queue database; 6 under review_required "
      "every "
      "completed claim has a review from another session.\n\n  History from before the cut-over is counted and never fails. The "
      "default cut-over is when migration\n  00042 was applied; --floor names a claim id instead.\n\n  Rule 5 grades a claim "
      "only if it ended at or after the evidence horizon: the later of the build "
      "queue database's\n  creation and now minus its tagged-history days (with no such database, when migration 00044 was "
      "applied). "
      "An earlier claim is\n  listed under queue_evidence_unavailable and not graded on rule 5. When planar.db has no record of "
      "migration "
      "44 and\n  the build queue database is absent, every gated claim is listed under queue_evidence_unavailable and none "
      "fails.\n\n  Exit codes:\n    0  the "
      "trail broke no rule\n    1  at least one violation, or the plan, its policy or the build queue database could not be "
      "read");
  add_positional(*conformance, "plan-id", "Plan id");
  add_string(*conformance, "--scope", "Require the plan to be in this scope slug");
  add_int(*conformance, "--floor", "Grade claims with this id or higher; replaces the data-derived cut-over");
  // Inherited from `health`, and LAST — see `declare_health`.
  add_json(*conformance, "Emit machine-readable JSON instead of text");
  return conformance;
}
} // namespace planar::cmd::handlers::health_cli
