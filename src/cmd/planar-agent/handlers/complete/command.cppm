/// @file command.cppm
/// @brief CLI declarations for planar-agent complete.
module;
export module planar.cmd.planar_agent.handlers.complete.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::complete_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* complete =
      root.add_subcommand("complete", "Atomically end the work session: task \xE2\x86\x92 done, claim \xE2\x86\x92 completed.");
  shared::add_claim(*complete, "Claim token returned by pull/claim");
  complete->add_option("--summary")->description("Free-text completion summary recorded on the action");
  shared::add_no_locality_probe(*complete, "Skip the git locality probe and commit collection");
  shared::add_supervision(*complete, true);
  shared::add_json(*complete, shared::k_undocumented);
}
} // namespace planar::cmd::agent::handlers::complete_cli
