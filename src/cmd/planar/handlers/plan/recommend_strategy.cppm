/// @file recommend_strategy.cppm
/// @brief CLI declaration for `planar plan recommend-strategy`.
export module planar.cmd.planar.handlers.plan.recommend_strategy;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the recommend strategy CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_recommend_strategy(CLI::App& plan) -> CLI::App* {
  CLI::App* recommend_strategy = plan.add_subcommand(
      "recommend-strategy",
      "Recommend an execution strategy for a plan's open (todo) tasks.\n\n  Applies the six parallel-eligibility rules (decision "
      "370) and\n  reports the parallel-eligible subset plus the serialized remainder\n  with per-task exclusion reasons:\n    "
      "1. no blocked_by chain to a not-done task\n    2. disjoint task_touches (empty touches = touches-everything)\n    3. no "
      "schema migration touched\n    4. no singleton authoritative file touched\n    5. no open question linked\n    6. no "
      "proposed decision linked\n\n  --closure-source selects the signal rule 2's overlap test reads\n  (decision D4): "
      "'declared' (default) uses the declared task_touches and\n  is byte-for-byte the pre-D4 behavior; 'derived' uses the "
      "computed\n  symbol-level closure (closures table) so two tasks overlap when their\n  derived closures share a symbol even "
      "when their declared files differ.\n\n  READ-ONLY: computes and reports; writes nothing. fan_out_available\n  is true when "
      ">= 2 tasks are eligible.");
  add_string_default(*recommend_strategy, "--closure-source", "declared",
                     "Rule-2 overlap signal: 'declared' (default, task_touches) or 'derived' (computed closure).");
  add_json(*recommend_strategy, "Emit machine-readable JSON instead of text");
  add_positional(*recommend_strategy, "plan-id", "Plan id or slug");
  return recommend_strategy;
}
} // namespace planar::cmd::handlers::plan_cli
