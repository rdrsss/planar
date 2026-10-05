/// @file edit.cppm
/// @brief CLI declaration for `planar plan edit`.
export module planar.cmd.planar.handlers.plan.edit;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::plan_cli {
/// @brief Register the edit CLI node.
/// @param plan Input plan.
/// @return Registered CLI node.
export auto attach_edit(CLI::App& plan) -> CLI::App* {
  CLI::App* edit = plan.add_subcommand("edit", "Edit a plan in $EDITOR (editor-first flow).");
  add_bool(*edit, "--no-pull", "Accepted for parity; the handler does not read it");
  add_json(*edit, "Accepted for parity; the handler does not read it");
  add_positional(*edit, "plan-id", "Plan id or slug");
  return edit;
}
} // namespace planar::cmd::handlers::plan_cli
