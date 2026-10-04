/// @file validate.cppm
/// @brief CLI declaration for `resume validate`.
export module planar.cmd.planar.handlers.resume.validate;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::resume_cli {
/// @brief Register the validate CLI node.
/// @param resume Input resume.
/// @return Registered CLI node.
export auto attach_validate(CLI::App* resume) -> CLI::App* {
  CLI::App* validate = resume->add_subcommand("validate", "Check if a task is resumable.");
  add_json(*validate, k_undocumented);
  add_positional(*validate, "task-id", k_undocumented);
  return validate;
}
} // namespace planar::cmd::handlers::resume_cli
