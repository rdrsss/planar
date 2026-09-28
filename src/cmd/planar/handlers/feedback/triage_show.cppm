/// @file triage_show.cppm
/// @brief CLI declaration for `feedback triage show`.
export module planar.cmd.planar.handlers.feedback.triage_show;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::feedback_cli {
/// @brief Register the triage show CLI node.
/// @param triage Input triage.
/// @return Registered CLI node.
export auto attach_triage_show(CLI::App* triage) -> CLI::App* {
  CLI::App* show = triage->add_subcommand("show", "Show a triaged finding.");
  add_json(*show);
  add_positional(*show, "finding");
  return show;
}
} // namespace planar::cmd::handlers::feedback_cli
