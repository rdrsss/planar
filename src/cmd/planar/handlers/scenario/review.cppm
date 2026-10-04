/// @file review.cppm
/// @brief CLI declaration for `scenario review`.
export module planar.cmd.planar.handlers.scenario.review;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
/// @brief Register the review CLI node.
/// @param scenario Input scenario.
/// @return Registered CLI node.
export auto attach_review(CLI::App& scenario) -> CLI::App* {
  CLI::App* review = scenario.add_subcommand("review", "Reviewer entry point for scenario diff.");
  add_bool(*review, "--approve", k_undocumented);
  add_bool(*review, "--request-changes", k_undocumented);
  add_json(*review, k_undocumented);
  add_positional(*review, "scenario-id", k_undocumented);
  return review;
}
} // namespace planar::cmd::handlers::scenario_cli
