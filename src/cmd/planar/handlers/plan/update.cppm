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
  add_string(*update, "--title", k_undocumented);
  add_string(*update, "--slug", k_undocumented);
  add_string(*update, "--summary", k_undocumented);
  add_string(*update, "--status", k_undocumented);
  add_int(*update, "--parent", k_undocumented);
  add_string(*update, "--scope", k_undocumented);
  add_json(*update, k_undocumented);
  add_positional(*update, "plan-id", k_undocumented);
  return update;
}
} // namespace planar::cmd::handlers::plan_cli
