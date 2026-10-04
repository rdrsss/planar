/// @file next.cppm
/// @brief CLI declaration for `planar plan next`.
export module planar.cmd.planar.handlers.plan.next;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the next CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_next(CLI::App& plan) -> CLI::App* {
  CLI::App* next = plan.add_subcommand(
      "next", "Bucketed claim-aware view of next work on a plan.\n\n  Buckets:\n    available  task is todo (or doing without an "
              "active claim)\n               and ready to be pulled\n    claimed    task has an active unexpired claim\n    "
              "stale      task has a stale claim (reconcile or lease-expired)\n    blocked    task status is blocked\n\n  "
              "Without --include-claimed / --include-stale the text rendering\n  shows only the available + blocked buckets "
              "\xe2\x80\x94 the JSON shape always\n  carries every bucket.");
  add_bool(*next, "--include-claimed", "Show the claimed bucket in text mode (JSON always includes it).");
  add_bool(*next, "--include-stale", "Show the stale bucket in text mode (JSON always includes it).");
  add_json(*next, k_undocumented);
  add_positional(*next, "plan-id", k_undocumented);
  return next;
}
} // namespace planar::cmd::handlers::plan_cli
