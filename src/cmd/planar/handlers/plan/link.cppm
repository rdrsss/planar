/// @file link.cppm
/// @brief CLI declaration for `planar plan link`.
export module planar.cmd.planar.handlers.plan.link;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the link CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_link(CLI::App& plan) -> CLI::App* {
  CLI::App* link = plan.add_subcommand("link", "Create an entity link from a plan to another entity.");
  add_string(*link, "--relationship");
  add_string(*link, "--scope");
  add_json(*link);
  add_positional(*link, "plan-id");
  add_positional(*link, "ref");
  return link;
}
} // namespace planar::cmd::handlers::plan_cli
