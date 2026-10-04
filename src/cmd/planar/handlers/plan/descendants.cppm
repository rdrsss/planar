/// @file descendants.cppm
/// @brief CLI declaration for `planar plan descendants`.
export module planar.cmd.planar.handlers.plan.descendants;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the descendants CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_descendants(CLI::App& plan) -> CLI::App* {
  CLI::App* descendants = plan.add_subcommand(
      "descendants", "Emit the anchor plan's full subtree (child plans + tasks) in\n  dependency-topological order (anchor "
                     "\xe2\x86\x92 child plans \xe2\x86\x92 tasks).\n\n  READ-ONLY: queries and reports; writes nothing.");
  add_json(*descendants, "Emit machine-readable JSON instead of text");
  add_positional(*descendants, "plan-id", "Plan id or slug");
  return descendants;
}
} // namespace planar::cmd::handlers::plan_cli
