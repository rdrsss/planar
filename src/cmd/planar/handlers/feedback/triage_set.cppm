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
  add_string_required(*set, "--severity", "Severity: info, low, medium, high, critical");
  add_string_required(
      *set, "--disposition",
      "Disposition: untriaged, needs-reproduction, accepted, retained-question, dismissed, reported-external, duplicate");
  add_string_required(*set, "--reproduction", "Reproduction status: not-run, reproduced, not-reproduced, inconclusive");
  add_string(*set, "--duplicate-of",
             "Finding ref this one duplicates (task:id or question:id); required for disposition duplicate");
  add_string(*set, "--evidence", "Redacted evidence summary text");
  add_string(*set, "--scope", "Scope slug to resolve against instead of the cwd-derived scope");
  add_json(*set, "Emit machine-readable JSON instead of text");
  add_positional(*set, "finding", "Finding ref (task:id or question:id)");
  return set;
}
} // namespace planar::cmd::handlers::feedback_cli
