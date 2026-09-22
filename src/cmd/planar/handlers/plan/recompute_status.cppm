/// @file recompute_status.cppm
/// @brief CLI declaration for `planar plan recompute-status`.
export module planar.cmd.planar.handlers.plan.recompute_status;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
export auto attach_recompute_status(CLI::App& plan) -> CLI::App* {
  CLI::App* recompute_status =
      plan.add_subcommand("recompute-status", "Recompute a plan's roll-up status (--plan <id> or --all).");
  add_int(*recompute_status, "--plan", "Recompute one plan by id.");
  add_bool(*recompute_status, "--all", "Recompute every plan in the DB.");
  add_json(*recompute_status);
  return recompute_status;
}
} // namespace planar::cmd::handlers::plan_cli
