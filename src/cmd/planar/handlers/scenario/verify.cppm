/// @file verify.cppm
/// @brief CLI declaration for `scenario verify`.
export module planar.cmd.planar.handlers.scenario.verify;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
export auto attach_verify(CLI::App& scenario) -> CLI::App* {
  CLI::App* verify = scenario.add_subcommand(
      "verify", "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass).");
  add_string(*verify, "--outcome");
  add_string(*verify, "--summary");
  add_json(*verify);
  add_positional(*verify, "scenario-id");
  return verify;
}
} // namespace planar::cmd::handlers::scenario_cli
