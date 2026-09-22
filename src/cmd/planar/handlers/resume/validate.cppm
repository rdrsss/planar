/// @file validate.cppm
/// @brief CLI declaration for `resume validate`.
export module planar.cmd.planar.handlers.resume.validate;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::resume_cli {
export auto attach_validate(CLI::App* resume) -> CLI::App* {
  CLI::App* validate = resume->add_subcommand("validate", "Check if a task is resumable.");
  add_json(*validate);
  add_positional(*validate, "task-id");
  return validate;
}
} // namespace planar::cmd::handlers::resume_cli
