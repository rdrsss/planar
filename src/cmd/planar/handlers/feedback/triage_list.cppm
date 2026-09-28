/// @file triage_list.cppm
/// @brief CLI declaration for `feedback triage list`.
export module planar.cmd.planar.handlers.feedback.triage_list;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::feedback_cli {
/// @brief Register the triage list CLI node.
/// @param triage Input triage.
/// @return Registered CLI node.
export auto attach_triage_list(CLI::App* triage) -> CLI::App* {
  CLI::App* list = triage->add_subcommand("list", "List triaged findings.");
  add_int(*list, "--plan");
  add_string(*list, "--severity");
  add_string(*list, "--disposition");
  add_json(*list);
  return list;
}
} // namespace planar::cmd::handlers::feedback_cli
