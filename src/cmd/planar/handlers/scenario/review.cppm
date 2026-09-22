/// @file review.cppm
/// @brief CLI declaration for `scenario review`.
export module planar.cmd.planar.handlers.scenario.review;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::scenario_cli {
export auto attach_review(CLI::App& scenario) -> CLI::App* {
  CLI::App* review = scenario.add_subcommand("review", "Reviewer entry point for scenario diff.");
  add_bool(*review, "--approve");
  add_bool(*review, "--request-changes");
  add_json(*review);
  add_positional(*review, "scenario-id");
  return review;
}
} // namespace planar::cmd::handlers::scenario_cli
