/// @file command.cppm
/// @brief CLI declarations for planar-agent review.
module;
export module planar.cmd.planar_agent.handlers.review.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::review_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* review =
      root.add_subcommand("review", "Record a review verdict on a claim, from a reviewer who is not the claim's own session.");
  shared::add_claim(*review, "Token of the claim under review (the coder's claim; active or ended)");
  // Plain strings with hand-rolled validators in the handler, NOT `required()` or a choice: a
  // missing or invalid value must refuse at exit 2 (a parse error exits 1), and no review row may
  // ever be written without a verdict.
  review->add_option("--verdict")->description("approve | request-changes (required)");
  review->add_option("--summary")->description("What the reviewer found, stored on the review action (required)");
  review->add_option("--reviewer-claim")
      ->description("The reviewer's own claim token; its vendor session is the caller's session. Required when the "
                    "reviewed claim has no vendor session");
  shared::add_vendor(*review, "Vendor tag recorded on the review when no reviewer claim names one",
                     "The reviewer's vendor session id; refused when it is the reviewed claim's. Used when --reviewer-claim "
                     "is not given");
  shared::add_json(*review, "Emit machine-readable JSON instead of text");
}
} // namespace planar::cmd::agent::handlers::review_cli
