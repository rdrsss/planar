/// @file verify.cppm
/// @brief CLI declaration for `scenario verify`.
export module planar.cmd.planar.handlers.scenario.verify;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the verify CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_verify(CLI::App& scenario) -> CLI::App* {
  CLI::App* verify = scenario.add_subcommand(
      "verify", "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass).");
  add_string(*verify, "--outcome", k_undocumented);
  add_string(*verify, "--summary", k_undocumented);
  add_json(*verify, k_undocumented);
  add_positional(*verify, "scenario-id", k_undocumented);
  return verify;
}
} // namespace planar::cmd::handlers::scenario_cli
