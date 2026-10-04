/// @file validate.cppm
/// @brief CLI declaration for `handoff validate`.
export module planar.cmd.planar.handlers.handoff.validate;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::handoff_cli {
/// @brief Register the validate CLI node.
/// @param handoff Input handoff.
/// @return Registered CLI node.
export auto attach_validate(CLI::App* handoff) -> CLI::App* {
  CLI::App* validate = handoff->add_subcommand("validate", "Validate a pending handoff.");
  add_string(*validate, "--vendor", k_undocumented);
  add_string(*validate, "--note", k_undocumented);
  add_json(*validate, k_undocumented);
  add_positional(*validate, "handoff-id", k_undocumented);
  return validate;
}
} // namespace planar::cmd::handlers::handoff_cli
