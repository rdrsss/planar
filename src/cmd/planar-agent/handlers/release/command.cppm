/// @file command.cppm
/// @brief CLI declarations for planar-agent release.
module;
export module planar.cmd.planar_agent.handlers.release.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::release_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* release = root.add_subcommand(
      "release", "Graceful give-up: task \xE2\x86\x92 todo, claim \xE2\x86\x92 released (vs fail's aborted).");
  shared::add_claim(*release, "Claim token returned by pull/claim");
  release->add_option("--reason")->description("Optional reason for releasing");
  shared::add_no_locality_probe(*release, "Skip the git locality probe and commit collection");
  shared::add_supervision(*release, true);
  shared::add_json(*release, shared::k_undocumented);
}
} // namespace planar::cmd::agent::handlers::release_cli
