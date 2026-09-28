/// @file evals.cppm
/// @brief CLI declaration for `models evals`.
export module planar.cmd.planar.handlers.models.evals;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::models_cli {
/// @brief Register the evals CLI node.
/// @param models Input models.
/// @return Registered CLI node.
export auto attach_evals(CLI::App& models) -> CLI::App* {
  CLI::App* evals = models.add_subcommand(
      "evals",
      "Evidence-backed candidate ranking over declared-experiment\n  terminal samples in the exact cohort (plan 950 task "
      "5530). Supply the\n  cohort flags to rank: results report sample and success counts, the raw\n  rate, the 95% Wilson "
      "lower bound, gate-failure rate, and expected excess\n  iterations. Candidates under --min-samples are labelled "
      "insufficient_data\n  and are never ranked or recommended; candidates below --quality-floor are\n  excluded before "
      "any iteration or gate-failure ordering, so a fast-but-wrong\n  candidate cannot outrank a slower correct one.\n\n  "
      "Without cohort flags this falls back to the LEGACY note-convention\n  scorecard below, which remains inspectable but "
      "is not evidence-backed:\n  it predates the routing evidence plane and carries no cohort or\n  independent-quality "
      "guarantee.\n\n  Legacy: read-only aggregation (plan 898/904, tech-spec 520 D8) over the\n  `dispatch_shape` / "
      "`model_choice` note convention in `session_entries`\n  (agents/orchestrator.md step 8a), joined with "
      "`agent_work_claims`\n  (terminal disposition) and `agent_actions` (test-coder expansion\n  outcome). Emits a "
      "per-(work-type, candidate) scorecard and a\n  recommended routing-map change. A pair with no completed-dispatch\n  "
      "history reports insufficient-data rather than a fabricated score.\n  Writes nothing: no routing-map mutation, no "
      "database write. Applying\n  a recommendation is a separate, explicit operator-gated action.");
  add_string(*evals, "--vendor", "Cohort vendor; enables evidence-backed ranking");
  add_string(*evals, "--role", "Cohort role");
  add_string(*evals, "--tier", "Cohort tier (small|medium|large)");
  add_string(*evals, "--work-type", "Cohort work type");
  add_string(*evals, "--complexity", "Cohort complexity (bounded|standard|high-risk)");
  add_string(*evals, "--project", "Cohort project id");
  add_string(*evals, "--validation-policy", "Cohort validation policy version");
  add_string(*evals, "--routing-policy", "Cohort routing policy version");
  add_string(*evals, "--min-samples", "Minimum samples before a candidate is ranked (default 5)");
  add_string(*evals, "--quality-floor", "Wilson lower-bound floor (default 0.5)");
  add_json(*evals);
  return evals;
}
} // namespace planar::cmd::handlers::models_cli
