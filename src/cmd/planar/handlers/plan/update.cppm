/// @file update.cppm
/// @brief CLI declaration for `planar plan update`.
export module planar.cmd.planar.handlers.plan.update;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the update CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_update(CLI::App& plan) -> CLI::App* {
  CLI::App* update = plan.add_subcommand("update", "Update mutable fields on a plan.");
  add_string(*update, "--title", "New title for the plan");
  add_string(*update, "--slug", "New slug for the plan");
  add_string(*update, "--summary", "New summary text; a leading @ reads it from a file");
  add_string(*update, "--status", "New status: draft, active, paused, done, abandoned");
  add_int(*update, "--parent", "New parent plan id");
  add_string(*update, "--scope", "Scope slug to move the plan to");
  add_json(*update, "Emit machine-readable JSON instead of text");
  add_positional(*update, "plan-id", "Plan id or slug");
  return update;
}
} // namespace planar::cmd::handlers::plan_cli
