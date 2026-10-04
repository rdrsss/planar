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
  add_string_required(*set, "--severity", k_undocumented);
  add_string_required(*set, "--disposition", k_undocumented);
  add_string_required(*set, "--reproduction", k_undocumented);
  add_string(*set, "--duplicate-of", k_undocumented);
  add_string(*set, "--evidence", k_undocumented);
  add_string(*set, "--scope", k_undocumented);
  add_json(*set, k_undocumented);
  add_positional(*set, "finding", k_undocumented);
  return set;
}
} // namespace planar::cmd::handlers::feedback_cli
