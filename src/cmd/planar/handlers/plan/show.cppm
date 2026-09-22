/// @file show.cppm
/// @brief CLI declaration for `planar plan show`.
export module planar.cmd.planar.handlers.plan.show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
export auto attach_show(CLI::App& plan) -> CLI::App* {
  CLI::App* show = plan.add_subcommand("show", "Show a plan's details, steps, and child plans.");
  add_json(*show);
  add_positional(*show, "plan-id");
  return show;
}
} // namespace planar::cmd::handlers::plan_cli
