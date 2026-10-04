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
  add_string(*list, "--scope", k_undocumented);
  add_string(*list, "--status", k_undocumented);
  add_int(*list, "--parent", k_undocumented);
  add_string(*list, "--touches", k_undocumented);
  add_json(*list, k_undocumented);
  return list;
}
} // namespace planar::cmd::handlers::plan_cli
