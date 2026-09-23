/// @file command.cppm
/// @brief CLI declarations for planar-agent claim-associate.
module;
export module planar.cmd.planar_agent.handlers.claim_associate.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.handlers.shared.cli;
namespace planar::cmd::agent::handlers::claim_associate_cli {
export auto add(CLI::App& root) -> void {
  // --- claim-associate ----------------------------------------------------
  CLI::App* claim_associate =
      root.add_subcommand("claim-associate", "Associate a pre-acquired active claim with a workflow run (and optional stage), "
                                             "and/or hand it to the engine supervisor. At least one of --run or --supervisor.");
  shared::add_claim(*claim_associate, "Claim token to associate");
  claim_associate->add_option("--run")->description("workflow_runs.id to stamp on the claim")->check(cliapp::zig_int_validator());
  claim_associate->add_option("--stage")->description("Stage name to record (e.g. code, review); omit for NULL");
  claim_associate->add_option("--supervisor")
      ->description("engine: hand the claim to the engine supervisor (one-way; needs --attempt). caller: assert it is still "
                    "caller-supervised")
      ->check(CLI::IsMember{std::vector<std::string>{"caller", "engine"}});
  claim_associate->add_option("--attempt")->description("The Centurion attempt supervising the claim (with --supervisor engine)");
  shared::add_json(*claim_associate);
}
} // namespace planar::cmd::agent::handlers::claim_associate_cli
