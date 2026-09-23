/// @file triage_set.cppm
/// @brief CLI declaration for `feedback triage set`.
export module planar.cmd.planar.handlers.feedback.triage_set;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::feedback_cli {
/// @brief Register the triage set CLI node.
/// @param triage Input triage.
/// @return Registered CLI node.
export auto attach_triage_set(CLI::App* triage) -> CLI::App* {
  CLI::App* set = triage->add_subcommand("set", "Set operator-confirmed triage fields.");
  add_string_required(*set, "--severity");
  add_string_required(*set, "--disposition");
  add_string_required(*set, "--reproduction");
  add_string(*set, "--duplicate-of");
  add_string(*set, "--evidence");
  add_string(*set, "--scope");
  add_json(*set);
  add_positional(*set, "finding");
  return set;
}
} // namespace planar::cmd::handlers::feedback_cli
