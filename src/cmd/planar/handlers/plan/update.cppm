/// @file update.cppm
/// @brief CLI declaration for `planar plan update`.
export module planar.cmd.planar.handlers.plan.update;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
export auto attach_update(CLI::App& plan) -> CLI::App* {
  CLI::App* update = plan.add_subcommand("update", "Update mutable fields on a plan.");
  add_string(*update, "--title");
  add_string(*update, "--slug");
  add_string(*update, "--summary");
  add_string(*update, "--status");
  add_int(*update, "--parent");
  add_string(*update, "--scope");
  add_json(*update);
  add_positional(*update, "plan-id");
  return update;
}
} // namespace planar::cmd::handlers::plan_cli
