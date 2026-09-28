/// @file command.cppm
/// @brief CLI declarations for planar-agent fail.
module;
export module planar.cmd.planar_agent.handlers.fail.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::fail_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* fail =
      root.add_subcommand("fail", "Atomically fail the work session: task \xE2\x86\x92 todo, claim \xE2\x86\x92 aborted.");
  shared::add_claim(*fail, "Claim token returned by pull/claim");
  fail->add_option("--reason")->description("Failure reason recorded on the claim and action")->required();
  fail->add_option("--category")
      ->description("Closed failure category (default: unknown)")
      ->check(CLI::IsMember{shared::failure_categories()})
      ->default_str("unknown");
  shared::add_no_locality_probe(*fail, "Skip the git locality probe and commit collection");
  shared::add_supervision(*fail, true);
  shared::add_json(*fail);
}
} // namespace planar::cmd::agent::handlers::fail_cli
