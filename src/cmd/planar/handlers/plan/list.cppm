/// @file list.cppm
/// @brief CLI declaration for `planar plan list`.
export module planar.cmd.planar.handlers.plan.list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
export auto attach_list(CLI::App& plan) -> CLI::App* {
  CLI::App* list = plan.add_subcommand("list", "List plans.");
  add_string(*list, "--scope");
  add_string(*list, "--status");
  add_int(*list, "--parent");
  add_string(*list, "--touches");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::plan_cli
