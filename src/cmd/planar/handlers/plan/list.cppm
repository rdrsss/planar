/// @file list.cppm
/// @brief CLI declaration for `planar plan list`.
export module planar.cmd.planar.handlers.plan.list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the list CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_list(CLI::App& plan) -> CLI::App* {
  CLI::App* list = plan.add_subcommand("list", "List plans.");
  add_string(*list, "--scope", "Restrict to this scope slug instead of the cwd-derived scope");
  add_string(*list, "--status", "Restrict to a status or comma-separated list: draft, active, paused, done, abandoned");
  add_int(*list, "--parent", "Restrict to children of this parent plan id");
  add_string(*list, "--touches", "Restrict to plans scoped to or touching this repo slug");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::plan_cli
