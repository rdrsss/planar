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
  add_string(*link, "--relationship",
             "Link relationship: derives-from, depends-on, addresses, verifies, cites, supersedes, touches");
  add_string(*link, "--scope", "Accepted but not read by this verb; no scope check is made");
  add_json(*link, "Emit machine-readable JSON instead of text");
  add_positional(*link, "plan-id", "Plan id or slug");
  add_positional(*link, "ref", "Target entity ref (kind:id)");
  return link;
}
} // namespace planar::cmd::handlers::plan_cli
