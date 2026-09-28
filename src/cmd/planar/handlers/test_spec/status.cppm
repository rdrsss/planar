/// @file status.cppm
/// @brief CLI declaration for `test_spec status`.
export module planar.cmd.planar.handlers.test_spec.status;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::test_spec_cli {
/// @brief Register the status CLI node.
/// @param test_spec Input test_spec.
/// @return Registered CLI node.
export auto attach_status(CLI::App* test_spec) -> CLI::App* {
  CLI::App* status = test_spec->add_subcommand("status", "Print per-milestone test-spec coverage for an anchor plan.");
  add_json(*status);
  add_positional_described(*status, "plan", "Plan slug or numeric id");
  return status;
}
} // namespace planar::cmd::handlers::test_spec_cli
